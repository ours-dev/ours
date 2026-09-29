/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_BINARY_HPP
#define EFTL_BINARY_HPP 1

#include <ours/types.hpp>
#include <ustl/option.hpp>

namespace eftl {
class Binary {
  public:
    enum class FormatId : u32 {
        Elf = 1,
        PE = 2,
        MachO = 3,
    };

    virtual ~Binary() = default;

    [[nodiscard]]
    virtual auto format() const noexcept -> FormatId = 0;

    [[nodiscard]]
    virtual auto entrypoint() const noexcept -> u64 = 0;

    [[nodiscard]]
    virtual auto imagebase() const noexcept -> u64 = 0;

    [[nodiscard]]
    virtual auto virtual_address_to_offset(u64 virtual_address) const noexcept
        -> ustl::Option<usize> = 0;
};
} // namespace eftl

#endif // EFTL_BINARY_HPP
