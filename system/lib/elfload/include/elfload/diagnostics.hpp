/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ELFLOAD_DIAGNOSTICS_HPP
#define ELFLOAD_DIAGNOSTICS_HPP 1

#include <string_view>

#include <ours/types.hpp>
#include <ustl/option.hpp>

namespace elfload {
/// How much a report weighs.
enum class Severity : u8 {
    /// The call is about to fail because of it, with the status it returns.
    Error,
    /// Not a failure: a fact worth having beside a later fault, such as where
    /// an import was bound.
    Note,
};

/// One thing elfload has to say.
///
/// Nothing in it is formatted. The text is fixed, and the name and the number
/// that complete it travel beside it, so that elfload uses no formatting
/// library -- and none of what one drags into a program: logz4 alone pulls in
/// libc++'s floating-point formatting, an allocator and a linker section that
/// only the kernel provides -- and the caller writes the three however its
/// output goes.
struct Report {
    Severity severity = Severity::Error;
    /// What happened, in words that read on their own.
    std::string_view what;
    /// The symbol it is about, or the ELF parser's own message; empty if none.
    std::string_view name = {};
    /// An address, an index or a relocation type that |what| refers to.
    ustl::Option<u64> value = {};
};

/// Where elfload's reports go: the caller decides what one becomes -- a line
/// in the kernel's log, a line on a program's debug console, or nothing. As
/// Zircon's elfldltl takes a Diagnostics object in every call that can fail
/// (lib/elfldltl/diagnostics.h), and for the reason ImageWriter is a function
/// pointer and its argument: one piece of code, whatever is listening.
struct Diagnostics {
    auto (*report)(void *context, Report const &report) -> void = nullptr;
    void *context = nullptr;

    /// Hands |what| on, if anyone listens.
    auto operator()(Report const &what) const -> void {
        if (report != nullptr) {
            report(context, what);
        }
    }
};
} // namespace elfload

#endif // #ifndef ELFLOAD_DIAGNOSTICS_HPP
