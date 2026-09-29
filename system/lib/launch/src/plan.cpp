/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <launch/plan.hpp>

namespace launch {
auto plan_mappings(std::span<Segment const> segments, Relro relro, u64 file_size, Plan &out)
    -> ours::Status {
    // Yours to write; the specification is the comment on this function in
    // <launch/plan.hpp>, and test/plan_test.cpp is the check.
    return ours::Status::Unimplemented;
}
} // namespace launch
