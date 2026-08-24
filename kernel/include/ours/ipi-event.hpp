/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IPI_EVENT_HPP
#define OURS_IPI_EVENT_HPP 1

#include <ours/types.hpp>

namespace ours {
    enum class IpiEvent : u8 {
        Resched,
        Interrupt,
        Suspend,
    };
} // namespace ours

#endif // OURS_IPI_EVENT_HPP
