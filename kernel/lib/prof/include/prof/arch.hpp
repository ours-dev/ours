/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_ARCH_HPP
#define PROF_ARCH_HPP 1

#include <ours/status.hpp>

#include <optional>

namespace prof {
struct SampleContext;

/// Architecture seam implemented per-arch (kernel/arch/<x>/kernel/prof_*.cpp)
/// so the sampler core stays arch-neutral. The kernel links exactly one.
/// Every platform-specific fact `Sampler` needs — PMU control, the current
/// CPU, a timestamp, a place to write diagnostic bytes — is read through
/// this seam; nothing under `prof/` reaches for `ours::`/`arch::` directly.
namespace arch {

/// True when the machine exposes a usable architectural PMU.
[[nodiscard]]
auto pmu_present() -> bool;

/// Program the sampling counter on every online CPU and unmask its
/// interrupt. Must fail gracefully (leaving the sampler off) when the PMU
/// is absent.
auto start_pmu_sampling(u64 period) -> ours::Status;

/// Disable the sampling counter on every online CPU; each CPU acknowledges
/// by incrementing the quiesce counter once its own counter is off.
auto stop_pmu_sampling() -> void;

/// Capture the interrupted context on this CPU (tick-fallback source).
/// Returns nullopt when no trap frame is active.
[[nodiscard]]
auto capture_sample_context() -> std::optional<SampleContext>;

/// The CPU executing on entry, used to pick this CPU's sample buffer and to
/// tag records. Cheap; callable from interrupt context.
[[nodiscard]]
auto current_cpu() -> u32;

/// A monotonic per-record timestamp (TSC on x86). No defined unit across
/// architectures; used only to order samples for display.
[[nodiscard]]
auto timestamp() -> u64;

/// Writes `len` raw bytes to this machine's diagnostic output (COM1 on
/// x86). Used only by `Sampler::export_dump`: blocking, not interrupt-safe,
/// and called only after sampling has stopped.
auto debug_write(char const *bytes, usize len) -> void;

} // namespace arch

} // namespace prof

#endif // #ifndef PROF_ARCH_HPP
