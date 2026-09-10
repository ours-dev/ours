/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_TYPES_HPP
#define OURS_IRQ_TYPES_HPP 1

#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours::irq {
inline constexpr auto INVALID_V_IRQ_NUM = static_cast<VIrqNum>(-1);

enum class IrqReturn : u8 {
    None,
    Handled,
};

enum class IrqFlags : u32 {
    None,
    Shared = 0_bit,
    Permanent = 1_bit,
};
USTL_ENABLE_ENUM_BITMASK(IrqFlags);

using IrqHandler = auto (*)(VIrqNum, void *) -> IrqReturn;
} // namespace ours::irq

#endif // OURS_IRQ_TYPES_HPP
