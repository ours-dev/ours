/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_HPET_HPP
#define OURS_ARCH_HPET_HPP 1

#include <ours/assert.hpp>
#include <ours/compiler.hpp>
#include <ours/init.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/time/types.hpp>

#include <ustl/views/span.hpp>

namespace ours {
/// Available after hpet_init() called.
inline ustl::views::Span<time::ClockEventDevice *> HPET_CLKEVT;

INIT_CODE
auto hpet_init(PhysAddr mmio) -> Status;

auto is_hpet_enabled() -> bool;

auto hpet_enable() -> Status;

auto hpet_disable() -> Status;

} // namespace ours

#endif // #ifndef OURS_ARCH_HPET_HPP