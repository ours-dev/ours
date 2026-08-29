/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_APIC_IOAPIC_HPP
#define ARCH_X86_APIC_IOAPIC_HPP 1

#include <arch/types.hpp>
#include <arch/x86/apic/common.hpp>
#include <arch/x86/interrupt.hpp>
#include <ours/status.hpp>

namespace arch {
class IoApicRouteEntry final {
  public:
    constexpr auto set_vector(IrqVec vector) noexcept -> IoApicRouteEntry & {
        return set_field(VECTOR_MASK, 0, static_cast<u32>(vector));
    }

    constexpr auto set_delivery_mode(ApicDeliveryMode mode) noexcept -> IoApicRouteEntry & {
        return set_field(DELIVERY_MODE_MASK, 8, static_cast<u8>(mode));
    }

    constexpr auto set_dest_mode(ApicDestinationMode mode) noexcept -> IoApicRouteEntry & {
        return set_field(DESTINATION_MODE_MASK, 11, static_cast<u8>(mode));
    }

    constexpr auto set_polarity(ApicInterruptPolarity polarity) noexcept -> IoApicRouteEntry & {
        return set_field(POLARITY_MASK, 13, static_cast<u8>(polarity));
    }

    constexpr auto set_trigger_mode(ApicTriggerMode mode) noexcept -> IoApicRouteEntry & {
        return set_field(TRIGGER_MODE_MASK, 15, static_cast<u8>(mode));
    }

    constexpr auto set_masked(bool masked) noexcept -> IoApicRouteEntry & {
        return set_field(MASK_MASK, 16, masked ? 1 : 0);
    }

    constexpr auto set_dest(u8 destination) noexcept -> IoApicRouteEntry & {
        return set_field(DESTINATION_MASK, 56, destination);
    }

    [[nodiscard]]
    constexpr auto value() const noexcept -> u64 {
        return value_;
    }

    constexpr auto set_value(u64 value) noexcept -> void {
        value_ = value;
    }

  private:
    static constexpr auto VECTOR_MASK = u64{0xFF};
    static constexpr auto DELIVERY_MODE_MASK = u64{0x7};
    static constexpr auto DESTINATION_MODE_MASK = u64{0x1};
    static constexpr auto POLARITY_MASK = u64{0x1};
    static constexpr auto TRIGGER_MODE_MASK = u64{0x1};
    static constexpr auto MASK_MASK = u64{0x1};
    static constexpr auto DESTINATION_MASK = u64{0xFF};

    constexpr auto set_field(u64 mask, usize shift, u64 value) noexcept -> IoApicRouteEntry & {
        value_ = (value_ & ~(mask << shift)) | ((value & mask) << shift);
        return *this;
    }

    u64 value_{};
};

class IoApic final {
  public:
    enum class Register : u32 {
        Id = 0,
        Version = 1,
        RedirectionTable = 0x10,
    };

    auto init(PhysAddr physical_base, VirtAddr, u32 global_irq_base) noexcept -> IoApic & {
        physical_base_ = physical_base;
        global_irq_base_ = global_irq_base;
        return *this;
    }

    auto reset_mmio(VirtAddr virtual_base) noexcept -> void {
        mmio_base_ = reinterpret_cast<volatile u32 *>(virtual_base);
        id_ = (read(Register::Id) >> 24) & 0xF;
        auto const version = read(Register::Version);
        version_ = version & 0xFF;
        max_redirection_index_ = (version >> 16) & 0xFF;
    }

    [[nodiscard]]
    auto id() const noexcept -> u32 {
        return id_;
    }
    [[nodiscard]]
    auto version() const noexcept -> u32 {
        return version_;
    }
    [[nodiscard]]
    auto phys_base() const noexcept -> PhysAddr {
        return physical_base_;
    }
    [[nodiscard]]
    auto virt_base() const noexcept -> VirtAddr {
        return reinterpret_cast<VirtAddr>(mmio_base_);
    }
    [[nodiscard]]
    auto gsi_base() const noexcept -> usize {
        return global_irq_base_;
    }
    [[nodiscard]]
    auto max_local_irqnum() const noexcept -> usize {
        return max_redirection_index_;
    }
    [[nodiscard]]
    auto irq_count() const noexcept -> usize {
        return static_cast<usize>(max_redirection_index_) + 1;
    }

    [[nodiscard]]
    auto owns_global_irq(HIrqNum irq) const noexcept -> bool {
        auto const base = static_cast<HIrqNum>(global_irq_base_);
        return irq >= base && irq - base < irq_count();
    }

    [[nodiscard]]
    auto read(Register address) const noexcept -> u32 {
        *control_register(IO_REG_SEL) = static_cast<u32>(address);
        return *control_register(IO_WINDOW);
    }

    auto write(Register address, u32 value) noexcept -> void {
        *control_register(IO_REG_SEL) = static_cast<u32>(address);
        *control_register(IO_WINDOW) = value;
    }

    auto issue_eoi(HIrqNum irq) noexcept -> ours::Status {
        if (version_ < EOI_REGISTER_MIN_VERSION) {
            return ours::Status::NotSupported;
        }
        *control_register(EOI_REGISTER) = static_cast<u32>(irq);
        return ours::Status::Ok;
    }

    auto mask_irq(HIrqNum irq) noexcept -> void {
        auto entry = read_redirection(irq);
        entry.set_masked(true);
        write_redirection(irq, entry);
    }

    auto unmask_irq(HIrqNum irq) noexcept -> void {
        auto entry = read_redirection(irq);
        entry.set_masked(false);
        write_redirection(irq, entry);
    }

    auto config_irq(HIrqNum irq, ApicTriggerMode trigger, ApicInterruptPolarity polarity,
                    ApicDeliveryMode delivery, ApicDestinationMode destination_mode, u8 destination,
                    IrqVec vector, bool masked = true) noexcept -> void {
        IoApicRouteEntry entry;
        entry.set_vector(vector)
            .set_delivery_mode(delivery)
            .set_dest_mode(destination_mode)
            .set_polarity(polarity)
            .set_trigger_mode(trigger)
            .set_dest(destination)
            .set_masked(masked);
        write_redirection(irq, entry);
    }

  private:
    static constexpr auto IO_REG_SEL = usize{0};
    static constexpr auto IO_WINDOW = usize{0x10};
    static constexpr auto EOI_REGISTER = usize{0x40};
    static constexpr auto EOI_REGISTER_MIN_VERSION = u32{0x20};

    [[nodiscard]]
    auto control_register(usize offset) const noexcept -> volatile u32 * {
        return reinterpret_cast<volatile u32 *>(reinterpret_cast<volatile u8 *>(mmio_base_) +
                                                offset);
    }

    [[nodiscard]]
    auto redirection_index(HIrqNum irq) const noexcept -> u32 {
        return static_cast<u32>(irq - static_cast<HIrqNum>(global_irq_base_));
    }

    [[nodiscard]]
    auto read_redirection(HIrqNum irq) const noexcept -> IoApicRouteEntry {
        auto const index = redirection_index(irq);
        auto const low =
            read(static_cast<Register>(static_cast<u32>(Register::RedirectionTable) + index * 2));
        auto const high = read(
            static_cast<Register>(static_cast<u32>(Register::RedirectionTable) + index * 2 + 1));
        IoApicRouteEntry entry;
        entry.set_value((static_cast<u64>(high) << 32) | low);
        return entry;
    }

    auto write_redirection(HIrqNum irq, IoApicRouteEntry const &entry) noexcept -> void {
        auto const index = redirection_index(irq);
        auto const value = entry.value();
        write(static_cast<Register>(static_cast<u32>(Register::RedirectionTable) + index * 2),
              static_cast<u32>(value));
        write(static_cast<Register>(static_cast<u32>(Register::RedirectionTable) + index * 2 + 1),
              static_cast<u32>(value >> 32));
    }

    volatile u32 *mmio_base_{};
    PhysAddr physical_base_{};
    u32 global_irq_base_{};
    u32 id_{};
    u32 version_{};
    u32 max_redirection_index_{};
};

struct IoApicIsaOverride final {
    u8 isa_irq{};
    bool remapped{};
    ApicTriggerMode tri_mode{ApicTriggerMode::Edge};
    ApicInterruptPolarity polarity{ApicInterruptPolarity::ActiveHigh};
    u32 global_irq{};
};
} // namespace arch

#endif // ARCH_X86_APIC_IOAPIC_HPP
