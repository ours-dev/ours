/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_IO_REGISTER_ADDR_HPP
#define ARCH_IO_REGISTER_ADDR_HPP 1

#include <arch/io/register_base.hpp>
#include <arch/types.hpp>

namespace arch::io {
/// Ties a `RegType` (a `RegisterBase<RegType, Layout>`-derived register
/// description) to a concrete numeric address, and produces `RegType`
/// instances bound to that address — either by reading hardware through
/// an IoProvider, or by wrapping an already-known raw value.
template <typename RegType>
class RegisterAddr {
    using Self = RegisterAddr;

  public:
    using ValueType = typename RegType::ValueType;

    constexpr explicit RegisterAddr(u32 addr) : m_addr(addr) {}

    // Instantiate a RegType using the value of the register read via the
    // given IO provider.
    template <typename IoProvider>
    auto read_from(IoProvider &io) const -> RegType {
        RegType reg{};
        reg.set_addr(m_addr);
        reg.read_from(io);
        return reg;
    }

    // Instantiate a RegType using the given value for the register.
    constexpr auto from_value(ValueType value) const -> RegType {
        RegType reg = RegType::from_value(value);
        reg.set_addr(m_addr);
        return reg;
    }

    constexpr auto addr() const -> u32 {
        return m_addr;
    }

  private:
    u32 const m_addr;
};
} // namespace arch::io

#endif // #ifndef ARCH_IO_REGISTER_ADDR_HPP
