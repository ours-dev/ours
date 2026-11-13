/// Copyright 2026 The Ours Authors
///

#ifndef OURS_ASSERT_HPP
#define OURS_ASSERT_HPP 1

// The assertion macros a program and the kernel share, and the
// bounded-formatting front end they share them with. The failure path is the
// same everywhere: panic() and panic_assertion() render into a
// never-allocating stack buffer (PanicSink) and hand the result to
// ours::vpanic(std::string_view), a hook each environment defines. The
// kernel keeps vpanic's other overload, the panic state a debugger asks
// about, and unreachable()/unimplement() in <ours/panic.hpp>
// (kernel/include/ours/panic.hpp), which is not on the user-side include
// path.

#include <ours/compiler.hpp>

#include <format>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ours {
/// Where a failed ASSERT ends: each environment defines it. The kernel and
/// phys in kernel/top/panic.cpp and kernel/phys/panic.cpp; a host test in
/// its own TU (system/lib/profdata/test/profdata_test.cpp). PHYS_EXPORT
/// because the definition lives in another image of the same build (a phys
/// module, or the kernel proper) and must stay reachable from it.
PHYS_EXPORT [[noreturn]]
auto vpanic(std::string_view fmt) -> void;

/// Bounds-checked stack sink for panic formatting: never allocates, so
/// panic() works in heap-less environments (phys).  Truncation is
/// acceptable for a panic path. The write that fills it is
/// detail::render()'s, through the bounded direct-pointer shape — the
/// sink's own write cursor advances out of the format's answer, and the
/// bound is where a message too long for the buffer is cut.
struct PanicSink {
    char *m_out;
    char *m_end;
};

namespace detail {
    /// Hands one panic argument to std::make_format_args in a shape this
    /// build's libc++ actually formats.
    ///
    /// A character *array* -- which is what every string literal is, and what
    /// ASSERT's stringified condition is -- reaches the argument store as an
    /// array type rather than as `const char*`, and comes out of the formatter
    /// as nothing at all: an assertion printed "Assertion failed on " and
    /// stopped, with the condition it had just failed on missing. A pointer or
    /// a string_view formats correctly, so decay the array to a string_view on
    /// the way in. Enums without a formatter use their underlying integers;
    /// everything else is forwarded untouched.
    ///
    /// ustl::fmt::decay_format_arg() is the same function, and logz4 uses it
    /// for the same reason. It is not shared with this file: panic.hpp lives
    /// in system/public and is included by translation units that have no ustl
    /// on their include path at all -- kernel/lib/ccrt/compiler/itanium.cpp
    /// and kernel/lib/libc/libcxx_format_glue.cpp among them -- so a
    /// dependency here would not build.
    template <typename T>
    FORCE_INLINE
    constexpr auto decay_format_arg(T &&value) -> decltype(auto) {
        using Bare = std::remove_cvref_t<T>;
        if constexpr (std::is_array_v<Bare> && std::is_same_v<std::remove_extent_t<Bare>, char>) {
            return std::string_view(value);
        } else if constexpr (std::is_enum_v<Bare> && !std::formattable<Bare, char>) {
            return std::to_underlying(value);
        } else {
            return static_cast<T &&>(value);
        }
    }
} // namespace detail

namespace detail {
    /// Renders one format string and its arguments onto the end of `sink`.
    ///
    /// The inner lambda is what gives make_format_args() the lvalues it
    /// requires: decay_format_arg() may hand back a prvalue string_view, and
    /// binding that to the lambda's own parameter keeps it alive for the
    /// whole formatting call.
    template <typename... Args>
    FORCE_INLINE
    auto render(PanicSink &sink, std::string_view fmt, Args &&...args) -> void {
        [&](auto &&...prepared) {
            // The write goes through the direct-pointer shape, the one
            // bounded write the standard machinery runs without an allocator:
            // vformat_to over the sink's iterator buffers through one that
            // silently grows past its first 256 bytes with operator new, and
            // a panic message long enough to cross it would recurse into the
            // very fault it is reporting. Truncation is acceptable here, and
            // the bound is where it happens.
            auto const written = std::__vformat_to_n<std::format_context>(
                sink.m_out, sink.m_end - sink.m_out, fmt, std::make_format_args(prepared...));
            sink.m_out = written.out;
        }(decay_format_arg(static_cast<Args &&>(args))...);
    }
} // namespace detail

// Panic formats into a bounded, never-allocating stack buffer (PanicSink)
// in every environment — the phys bootloader included. libc++'s
// std::vformat_to machinery is header-only and freestanding-safe (the
// kernel's logz4 runs the very same code), so there is no reason for a
// boot-time panic to drop its arguments and print bare "{}"s.
template <typename... Args>
FORCE_INLINE [[noreturn]]
auto panic(std::string_view fmt, Args &&...args) -> void {
    if constexpr (sizeof...(args) == 0) {
        vpanic(fmt);
    } else {
        char buffer[512];
        auto sink = PanicSink{buffer, buffer + sizeof(buffer) - 1};
        detail::render(sink, fmt, static_cast<Args &&>(args)...);
        *sink.m_out = '\0';
        vpanic(std::string_view(buffer, static_cast<std::size_t>(sink.m_out - buffer)));
    }
}

/// What a bare ASSERT(condition) expands to.
FORCE_INLINE [[noreturn]]
inline auto panic_assertion(std::string_view condition) -> void {
    panic("Assertion failed on {}", condition);
}

/// What ASSERT() expands to when the caller supplied a message.
///
/// The condition and the message have to be rendered as *two* format strings,
/// one after the other, and that is the whole point of this function. ASSERT
/// used to fold them into one call:
///
///     ours::panic("Assertion failed on {} with message: {}", #condition, __VA_ARGS__)
///
/// which has two placeholders and, for any assertion whose message takes
/// arguments of its own, three or more arguments. std::format ignores the
/// surplus, so the message's own `{}`s were never filled in and its arguments
/// were silently dropped -- and the half-rendered text then went to
/// vpanic(), which parsed it *again*. An assertion such as
///
///     DEBUG_ASSERT(a == b, "Invoke {}() on mismatched CPU[{}]", __func__, cpu)
///
/// therefore died inside the formatter with "argument index value is too
/// large", and what reached the console was libc++'s abort message rather
/// than a word about which CPU had gone wrong.
template <typename... Args>
FORCE_INLINE [[noreturn]]
auto panic_assertion(std::string_view condition, std::string_view fmt, Args &&...args) -> void {
    char buffer[512];
    auto sink = PanicSink{buffer, buffer + sizeof(buffer) - 1};
    detail::render(sink, "Assertion failed on {} with message: ", condition);
    detail::render(sink, fmt, static_cast<Args &&>(args)...);
    *sink.m_out = '\0';
    vpanic(std::string_view(buffer, static_cast<std::size_t>(sink.m_out - buffer)));
}

// --- the macros ---

/// The message, when there is one, is a format string of its own -- it is
/// rendered with its own arguments and appended after the condition. Which of
/// the two panic_assertion() overloads runs is settled by overload
/// resolution on the argument count, not by an `if constexpr` on
/// `__VA_OPT__`: with `if constexpr` in a non-template context both branches
/// still have to compile, so the no-message expansion had to name a call it
/// would never make, and that call quietly went stale.
#define ASSERT(condition, ...)                                                                     \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ours::panic_assertion(#condition __VA_OPT__(, ) __VA_ARGS__);                          \
        }                                                                                          \
    } while (0)

/// Assertions follow KERNEL_DEBUG_LEVEL, the build's one debug switch: any
/// level above 0 turns them on. It is set by //kernel/products:kernel_debug and
/// //build/switch:debug_level_* from the kernel_debug_level toolchain argument,
/// and every kernel and phys switchset defines it.
///
/// This used to test a bare `DEBUG`, which nothing anywhere defines -- so
/// DEBUG_ASSERT expanded to nothing in *every* configuration, debug builds
/// included. That is how `MemCache::allocate`'s
/// `DEBUG_ASSERT(nid < MAX_NODES)` came to sit silently over an
/// out-of-bounds read that hung the boot instead of reporting it.
///
/// `if constexpr` rather than #if for the body: it keeps the condition and its
/// arguments compiled -- and therefore honest -- at debug level 0, so an
/// assertion cannot rot into something that no longer builds once the level is
/// turned up. That costs nothing at runtime; the discarded branch emits no code.
///
/// The outer #if is still needed for translation units that never got the
/// define at all (host tools, tests), where `if constexpr (KERNEL_DEBUG_LEVEL)`
/// would be a compile error rather than a silently-off feature.
#if defined(KERNEL_DEBUG_LEVEL)
#define DEBUG_ASSERT(condition, ...)                                                               \
    do {                                                                                           \
        if constexpr (KERNEL_DEBUG_LEVEL) {                                                        \
            ASSERT(condition, __VA_ARGS__);                                                        \
        }                                                                                          \
    } while (0)
#else
#define DEBUG_ASSERT(condition, ...)
#endif // KERNEL_DEBUG_LEVEL

} // namespace ours

#endif // OURS_ASSERT_HPP
