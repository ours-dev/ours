/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IPI_SERVICE_HPP
#define OURS_IPI_SERVICE_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/ipi_event.hpp>

namespace ours {
class IpiService {
    using Self = IpiService;

  public:
    using Event = IpiEvent;

    static auto init() -> void;

    static auto send_resched(CpuNum dest) -> void;

    static auto send_resched(CpuMask const &dest) -> void;

    static auto send_event(CpuNum dest, Event event) -> void;

    static auto send_event(CpuMask const &dests, Event event) -> void;
};
} // namespace ours

#endif // #ifndef OURS_IPI_SERVICE_HPP
