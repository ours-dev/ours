// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_GNU_STACK_HPP
#define EFTL_ELF_GNU_STACK_HPP 1

#include <eftl/elf/details/segment.hpp>
#include <eftl/elf/traits.hpp>

#include <ours/types.hpp>
#include <ustl/option.hpp>

namespace eftl::elf {
/// A copied permission request encoded by a PT_GNU_STACK program header.
template <typename Traits>
class GnuStack final {
  public:
    using Segment = typename Traits::Segment;

    [[nodiscard]]
    static auto from(Segment const &segment) noexcept -> ustl::Option<GnuStack> {
        if (segment.type() != SegType::GnuStack) {
            return ustl::NONE;
        }
        return ustl::some(GnuStack(segment.permissions(), static_cast<u64>(segment.p_memsz)));
    }

    [[nodiscard]]
    constexpr auto permissions() const noexcept -> SegPerms {
        return m_permissions;
    }

    /// Returns the optional ELF stack-size hint. Interpretation is platform policy.
    [[nodiscard]]
    constexpr auto size_hint() const noexcept -> u64 {
        return m_size_hint;
    }

  private:
    constexpr GnuStack(SegPerms permissions, u64 size_hint) noexcept
        : m_permissions(permissions), m_size_hint(size_hint) {}

    SegPerms m_permissions{SegPerms::None};
    u64 m_size_hint{0};
};
} // namespace eftl::elf

#endif // EFTL_ELF_GNU_STACK_HPP
