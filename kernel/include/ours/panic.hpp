/// SPDX-License-Identifier: MIT
/// Copyright (c) 2025 smallhuazi

#ifndef OURS_PANIC_HPP
#define OURS_PANIC_HPP 1

// The kernel's panic interface: vpanic's declarations and the panic state a
// debugger asks about. The formatting front end -- panic(), panic_assertion(),
// PanicSink -- lives in <ours/assert.hpp>, which is on the user side too;
// this file is not. It used to be the other way round: both lived in
// system/public/ours/panic.hpp, where the public surface does not hold
// kernel-only facilities. Zircon keeps panic out of zircon/system/public the
// same way: its public zircon/assert.h declares the failure hook, and each
// environment defines it.
//
// Every environment defines the hook both files end in: the kernel and phys
// in kernel/top/panic.cpp and kernel/phys/panic.cpp, a host test in its own
// TU (system/lib/profdata/test/profdata_test.cpp).

#include <ours/assert.hpp>
#include <ours/compiler.hpp>

namespace ours {

template <typename String, typename FormatArgs>
[[noreturn]]
auto vpanic(String fmt, FormatArgs const &args) -> void;

PHYS_EXPORT [[noreturn]]
auto vpanic(std::string_view fmt) -> void;

/// Whether some CPU has started a panic: from then on the machine is
/// stopping and printing its report, and the lockup detector neither starts
/// a report of its own beside it nor takes the quiet for a hang.
/// The kernel's (kernel/top/panic.cpp); phys has no such state to ask about.
[[nodiscard]]
auto panic_in_progress() -> bool;

/// Whether the panic in progress is this CPU's own: it claimed the report
/// and is printing it. The lockup detector's NMI lets such a CPU return to
/// its panic rather than parking it, which would lose that report.
/// The kernel's, like panic_in_progress().
[[nodiscard]]
auto this_cpu_in_panic() -> bool;

FORCE_INLINE [[noreturn]]
inline auto unreachable() -> void {
    panic("Unreachable");
}

FORCE_INLINE [[noreturn]]
inline auto unimplement() -> void {
    panic("Unimplement");
}

} // namespace ours

#endif // OURS_PANIC_HPP
