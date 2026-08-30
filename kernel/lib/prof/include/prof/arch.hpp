/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_ARCH_HPP
#define PROF_ARCH_HPP 1

#include <ours/status.hpp>

#include <cstddef>
#include <cstdint>

namespace prof {

/// Architecture seam implemented per-arch (kernel/arch/<x>/kernel/prof_*.cpp)
/// so the sampler core stays arch-neutral. The kernel links exactly one.
namespace arch {

/// True when the machine exposes a usable architectural PMU.
[[nodiscard]]
auto pmu_present() -> bool;

/// Program the sampling counter on every online CPU and unmask its
/// interrupt. Must fail gracefully (leaving the sampler off) when the PMU
/// is absent.
auto start_pmu_sampling(std::uint64_t period) -> ours::Status;

/// Disable the sampling counter on every online CPU; each CPU acknowledges
/// by incrementing the quiesce counter once its own counter is off.
auto stop_pmu_sampling() -> void;

} // namespace arch

} // namespace prof

#endif // #ifndef PROF_ARCH_HPP
