/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// What the report (report.cpp) asks of the checker (checker.cpp). The
/// library's own; nothing outside it includes this.
#ifndef LOCKUP_DETECTOR_SRC_CHECKER_HPP
#define LOCKUP_DETECTOR_SRC_CHECKER_HPP 1

#include <ours/types.hpp>

namespace lockup_detector::priv {
/// The beats this CPU has had so far, its heartbeat. Takes no lock and
/// reads only this CPU's own per-CPU data, so an NMI may ask for it.
[[nodiscard]]
auto this_cpu_heartbeat() -> u64;

} // namespace lockup_detector::priv

#endif // #ifndef LOCKUP_DETECTOR_SRC_CHECKER_HPP
