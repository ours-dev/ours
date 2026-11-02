/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "vdso_export.hpp"
#include <ours/time_values.hpp>
#include <ours/version.hpp>
#include <ours/version_build.hpp>

/// The trap, generated beside the other stubs and hidden: this file is the
/// symbol a program binds to.
extern "C" auto ours_clock_get_monotonic_via_kernel() -> ours::Instant;

/// Defined in vdso.S, in the read-only data, so the compiler cannot treat
/// the bytes as a constant it already knows.
// The spelling is the assembly symbol's. A rename would be a different object.
extern "C" ours::TimeValues ours_time_values; // NOLINT(readability-identifier-naming)

namespace {
auto load_u64(u64 const *const slot) -> u64 {
    return __atomic_load_n(slot, __ATOMIC_ACQUIRE);
}

auto snapshot() -> ours::TimeValues {
    auto const &page = ours_time_values;
    for (;;) {
        auto const seq = load_u64(&page.seq);
        if ((seq & 1U) != 0U) {
            continue;
        }
        ours::TimeValues copy{};
        copy.ticks = load_u64(&page.ticks);
        copy.nanoseconds =
            static_cast<i64>(load_u64(reinterpret_cast<u64 const *>(&page.nanoseconds)));
        copy.numerator = __atomic_load_n(&page.numerator, __ATOMIC_RELAXED);
        copy.denominator = __atomic_load_n(&page.denominator, __ATOMIC_RELAXED);
        copy.accessible = __atomic_load_n(&page.accessible, __ATOMIC_RELAXED);
        copy.counter = load_u64(&page.counter);
        if (load_u64(&page.seq) == seq) {
            return copy;
        }
    }
}

/// |counter_va| is a user virtual address of a 64-bit MMIO register, not a
/// compile-time constant. Relaxed so the compiler cannot fold the load.
auto sample_counter(u64 const counter_va) -> u64 {
    return __atomic_load_n(reinterpret_cast<u64 const *>(counter_va), __ATOMIC_RELAXED);
}
} // namespace

// Collected into .vdso_data. The spelling is the symbol a program does not
// bind; the functions below are the names it calls.
VDSO_EXPORT(u64, ours_published_ticks_hz, 0);     // NOLINT(readability-identifier-naming)
VDSO_EXPORT(i64, ours_published_boot_offset, 0);  // NOLINT(readability-identifier-naming)
VDSO_EXPORT(u64, ours_published_version_word, 1); // NOLINT(readability-identifier-naming)
// NOLINTNEXTLINE(readability-identifier-naming)
VDSO_EXPORT_STRING(ours_kernel_version_string, OURS_KERNEL_VERSION_STRING);

extern "C" [[gnu::visibility("default")]]
auto ours_ticks_per_second() -> u64 {
    return __atomic_load_n(&ours_published_ticks_hz, __ATOMIC_RELAXED);
}

extern "C" [[gnu::visibility("default")]]
auto ours_boot_offset() -> i64 {
    return __atomic_load_n(&ours_published_boot_offset, __ATOMIC_RELAXED);
}

extern "C" [[gnu::visibility("default")]]
auto ours_published_version() -> u64 {
    return __atomic_load_n(&ours_published_version_word, __ATOMIC_RELAXED);
}

extern "C" [[gnu::visibility("default")]]
auto ours_kernel_version() -> ours::KernelVersion {
    return ours_kernel_version_string;
}

extern "C" [[gnu::visibility("default")]]
auto ours_clock_get_monotonic() -> ours::Instant {
    auto const page = snapshot();
    if (page.accessible == 0 || page.counter == 0) {
        return ours_clock_get_monotonic_via_kernel();
    }
    return ours::monotonic_from(page, sample_counter(page.counter));
}
