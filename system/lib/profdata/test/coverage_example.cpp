/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "coverage_example.hpp"

namespace coverage_example {

auto classify(int const value) -> int {
    if (value < 0) {
        return -1;
    }
    if (value == 0) {
        return 0;
    }
    return 1;
}

auto never_called() -> int {
    return 42;
}

} // namespace coverage_example
