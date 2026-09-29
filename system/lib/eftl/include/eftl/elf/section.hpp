/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_SECTION_HPP
#define EFTL_ELF_SECTION_HPP 1

#include <eftl/binary_stream.hpp>

#include <ustl/option.hpp>

namespace eftl::elf {
class StringTableView final {
  public:
    [[nodiscard]]
    static auto from(ByteRange const bytes) noexcept -> ustl::Option<StringTableView> {
        if (!bytes.empty() && bytes.data() == nullptr) {
            return ustl::NONE;
        }
        return ustl::some(StringTableView(bytes));
    }

    [[nodiscard]]
    auto string_at(usize const offset) const noexcept -> ustl::Option<ByteRange> {
        if (offset >= m_bytes.size()) {
            return ustl::NONE;
        }
        for (usize index = offset; index < m_bytes.size(); ++index) {
            if (m_bytes.data()[index] == 0) {
                return ustl::some(ByteRange(m_bytes.data() + offset, index - offset));
            }
        }
        return ustl::NONE;
    }

  private:
    explicit constexpr StringTableView(ByteRange const bytes) noexcept : m_bytes(bytes) {}

    ByteRange m_bytes{};
};
} // namespace eftl::elf

#endif // EFTL_ELF_SECTION_HPP
