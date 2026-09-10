/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_TESTS_FAKE_ARCH_HPP
#define PROF_TESTS_FAKE_ARCH_HPP 1

#include <prof/sampler.hpp>

#include <optional>
#include <string>

/// Test-only stand-in for the per-arch `prof::arch` seam
/// (`prof/include/prof/arch.hpp`), so `prof::Sampler`'s own logic can be
/// exercised on the host without a real kernel underneath it. The actual
/// `prof::arch::*` functions are defined in `fake_arch.cpp`, driven by the
/// knobs below.
namespace prof_test {

/// Resets every knob to its default; call from each test's `SetUp()`.
auto reset() -> void;

auto set_pmu_present(bool present) -> void;
auto set_current_cpu(u32 cpu) -> void;
auto set_timestamp(u64 ts) -> void;
auto set_next_sample_context(std::optional<prof::SampleContext> context) -> void;

auto pmu_start_calls() -> int;
auto pmu_stop_calls() -> int;

/// Everything written through `arch::debug_write` since the last `reset()`.
auto debug_output() -> std::string const &;

} // namespace prof_test

#endif // #ifndef PROF_TESTS_FAKE_ARCH_HPP
