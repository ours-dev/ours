/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_APIC_COMMON_HPP
#define ARCH_X86_APIC_COMMON_HPP 1

#include <arch/types.hpp>

namespace arch {
// These values are the architectural encodings used in I/O APIC route
// entries and local-APIC ICRs, not arbitrary logical identifiers.
enum class ApicInterruptPolarity : u8 {
    ActiveHigh = 0,
    ActiveLow = 1,
};

enum class ApicDeliveryMode : u8 {
    Fixed = 0,
    LowestPriority = 1,
    SystemManagement = 2,
    NonMaskable = 4,
    Init = 5,
    Startup = 6,
    External = 7,
};

enum class ApicDestinationMode : u8 {
    Physical = 0,
    Logical = 1,
};

enum class ApicTriggerMode : u8 {
    Edge = 0,
    Level = 1,
};

enum class ApicDeliveryStatus : u8 {
    Idle = 0,
    Pending = 1,
};

enum class ApicIpiLevel : u8 {
    Deassert = 0,
    Assert = 1,
};

enum class ApicIpiTarget : u8 {
    One = 0,
    TheOne = One,
    Self = 1,
    All = 2,
    AllButSelf = 3,
};

[[nodiscard]]
constexpr auto to_string(ApicInterruptPolarity value) noexcept -> char const * {
    return value == ApicInterruptPolarity::ActiveHigh ? "high" : "low";
}

[[nodiscard]]
constexpr auto to_string(ApicDeliveryMode value) noexcept -> char const * {
    switch (value) {
        case ApicDeliveryMode::Fixed:
            return "fixed";
        case ApicDeliveryMode::LowestPriority:
            return "lowest-priority";
        case ApicDeliveryMode::SystemManagement:
            return "smi";
        case ApicDeliveryMode::NonMaskable:
            return "nmi";
        case ApicDeliveryMode::Init:
            return "init";
        case ApicDeliveryMode::Startup:
            return "startup";
        case ApicDeliveryMode::External:
            return "external";
    }
    return "unknown";
}

[[nodiscard]]
constexpr auto to_string(ApicDestinationMode value) noexcept -> char const * {
    return value == ApicDestinationMode::Physical ? "physical" : "logical";
}

[[nodiscard]]
constexpr auto to_string(ApicTriggerMode value) noexcept -> char const * {
    return value == ApicTriggerMode::Edge ? "edge" : "level";
}

class IpiRequest final {
  public:
    constexpr auto set_vector(u32 vector) noexcept -> IpiRequest & {
        return set_field(kVectorMask, 0, vector);
    }

    constexpr auto set_delivery_mode(ApicDeliveryMode mode) noexcept -> IpiRequest & {
        return set_field(kDeliveryModeMask, 8, static_cast<u8>(mode));
    }

    constexpr auto set_dest_mode(ApicDestinationMode mode) noexcept -> IpiRequest & {
        return set_field(kDestinationModeMask, 11, static_cast<u8>(mode));
    }

    constexpr auto set_delivery_status(ApicDeliveryStatus status) noexcept -> IpiRequest & {
        return set_field(kDeliveryStatusMask, 12, static_cast<u8>(status));
    }

    constexpr auto set_level(ApicIpiLevel level) noexcept -> IpiRequest & {
        return set_field(kLevelMask, 14, static_cast<u8>(level));
    }

    constexpr auto set_trigger_mode(ApicTriggerMode mode) noexcept -> IpiRequest & {
        return set_field(kTriggerModeMask, 15, static_cast<u8>(mode));
    }

    constexpr auto set_target(ApicIpiTarget target) noexcept -> IpiRequest & {
        return set_field(kTargetMask, 18, static_cast<u8>(target));
    }

    constexpr auto set_dest(u32 apic_id) noexcept -> IpiRequest & {
        return set_field(kDestinationMask, 56, apic_id);
    }

    [[nodiscard]]
    constexpr auto icr_low() const noexcept -> u32 {
        return static_cast<u32>(value_);
    }

    [[nodiscard]]
    constexpr auto icr_high() const noexcept -> u32 {
        return static_cast<u32>(value_ >> 32);
    }

  private:
    static constexpr auto kVectorMask = u64{0xFF};
    static constexpr auto kDeliveryModeMask = u64{0x7};
    static constexpr auto kDestinationModeMask = u64{0x1};
    static constexpr auto kDeliveryStatusMask = u64{0x1};
    static constexpr auto kLevelMask = u64{0x1};
    static constexpr auto kTriggerModeMask = u64{0x1};
    static constexpr auto kTargetMask = u64{0x3};
    static constexpr auto kDestinationMask = u64{0xFF};

    constexpr auto set_field(u64 mask, usize shift, u64 value) noexcept -> IpiRequest & {
        value_ = (value_ & ~(mask << shift)) | ((value & mask) << shift);
        return *this;
    }

    u64 value_{};
};

inline constexpr auto kNumIsaIrqs = usize{16};
} // namespace arch

#endif // ARCH_X86_APIC_COMMON_HPP
