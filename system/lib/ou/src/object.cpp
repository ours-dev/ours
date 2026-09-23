/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ou/object.hpp>

#include <ours/vdso.hpp>

namespace ou {
auto object_wait_one(ours::HandleValue const handle, ours::Signals const signals,
                     ours::Instant const deadline, ours::Signals *const observed) -> ours::Status {
    return ours_object_wait_one(handle, signals, deadline, observed);
}
} // namespace ou
