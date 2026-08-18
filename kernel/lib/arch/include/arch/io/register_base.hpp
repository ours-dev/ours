/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_IO_REGISTER_BASE_HPP
#define ARCH_IO_REGISTER_BASE_HPP 1

#include <arch/types.hpp>

namespace arch {

/// Pure bit-layout CRTP mixin shared by every register flavor (system
/// registers accessed via a dedicated instruction, MSRs, MMIO registers).
/// `Layout` is a `ustl::BitFields<Storage, Field<...>...>` instantiation
/// that owns the actual bit arithmetic; this class only forwards to it,
/// giving every derived register type a uniform get/set/value/from_value
/// API regardless of how its bits eventually reach hardware.
template <typename Derived, typename Layout>
class RegisterLayout {
    using Self = Derived;

  public:
    using ValueType = typename Layout::StorageUnit;

    template <auto Id>
    constexpr auto set(typename Layout::template ValueTypeOf<Id> value) -> Self & {
        layout_.template set<Id>(value);
        return static_cast<Self &>(*this);
    }

    template <auto Id>
    constexpr auto get() const -> typename Layout::template ValueTypeOf<Id> {
        return layout_.template get<Id>();
    }

    constexpr auto value() const -> ValueType {
        return layout_.template into<ValueType>();
    }

    static constexpr auto from_value(ValueType raw) -> Self {
        Self result{};
        result.layout_ = Layout::from_raw(raw);
        return result;
    }

  protected:
    Layout layout_{};
};

namespace io {

/// Address-dispatched register: identified by a numeric address/offset
/// (an MMIO register offset, an MSR number, ...) rather than by its own
/// type. Pairs with `RegisterAddr<Derived>` (see register_addr.hpp) and an
/// `IoProvider` — any type exposing
///   `template <typename Int> auto read(u32 addr) const -> Int;`
///   `template <typename Int> auto write(Int value, u32 addr) const -> void;`
/// (`arch::MsrIo` is one such provider).
template <typename Derived, typename Layout>
class RegisterBase : public RegisterLayout<Derived, Layout> {
    using Self = Derived;
    using Base = RegisterLayout<Derived, Layout>;

  public:
    using typename Base::ValueType;

    constexpr auto addr() const -> u32 {
        return addr_;
    }

    constexpr auto set_addr(u32 addr) -> void {
        addr_ = addr;
    }

    template <typename IoProvider>
    auto read_from(IoProvider &io) -> Self & {
        Base::layout_ = Layout::from_raw(io.template read<ValueType>(addr_));
        return static_cast<Self &>(*this);
    }

    template <typename IoProvider>
    auto write_to(IoProvider &io) -> Self & {
        io.write(Base::value(), addr_);
        return static_cast<Self &>(*this);
    }

  private:
    u32 addr_{};
};

} // namespace io
} // namespace arch

#endif // #ifndef ARCH_IO_REGISTER_BASE_HPP
