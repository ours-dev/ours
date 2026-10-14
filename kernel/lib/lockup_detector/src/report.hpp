/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// What the checker (checker.cpp) asks of the report (report.cpp). The
/// library's own; nothing outside it includes this.
#ifndef LOCKUP_DETECTOR_SRC_REPORT_HPP
#define LOCKUP_DETECTOR_SRC_REPORT_HPP 1

#include <lockup_detector/heartbeat_note.hpp>

#include <array>

#include <arch/interrupt.hpp>
#include <ours/cpu_mask.hpp>

namespace lockup_detector::priv {
/// A checking CPU's notes, one per CPU, indexed by CPU number. Its own note
/// is never written.
using Notes = std::array<HeartbeatNote, ours::MAX_CPU>;

/// Whether some CPU has claimed the report. The claim is never given back,
/// so once this is true it stays true, and a checker that sees it has
/// nothing left to do.
[[nodiscard]]
auto report_claimed() -> bool;

/// Claims the report for CPU |self|, which found CPU |stuck| without a beat
/// in its |notes|, and reports: every CPU's state, |stuck|'s last, then a
/// panic. |frame| is what |self|'s heartbeat timer interrupted, its own block.
///
/// Returns only if another CPU holds the claim already; that CPU's NMI then
/// asks this one for its state.
auto report_stuck(arch::IrqFrame const &frame, ours::CpuNum self, ours::CpuNum stuck,
                  Notes const &notes) -> void;

} // namespace lockup_detector::priv

#endif // #ifndef LOCKUP_DETECTOR_SRC_REPORT_HPP
