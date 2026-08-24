/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_DYNAMIC_ARRAY_HPP
#define EFTL_ELF_DYNAMIC_ARRAY_HPP 1

#include <eftl/binary_stream.hpp>
#include <eftl/elf/encoding.hpp>
#include <eftl/elf/table.hpp>
#include <eftl/elf/traits.hpp>

#include <ustl/option.hpp>

namespace eftl::elf {
// A checked, borrowed view of an ELF dynamic function-pointer array.
// The view decodes entries on access and never treats the byte range as an
// in-memory array, so unaligned and cross-endian input remains well-defined.
template <typename Traits>
class DynamicFunctionArrayView final {
  public:
    using Addr = typename Traits::Addr;
    using Entries = TableView<Addr>;

    [[nodiscard]]
    static auto from(ByteRange bytes, ByteOrder byte_order) noexcept
        -> ustl::Option<DynamicFunctionArrayView> {
        if (bytes.empty() || bytes.size() % sizeof(Addr) != 0) {
            return ustl::NONE;
        }
        auto const decoder = decoder_for<ScalarCodec<Addr>>(byte_order);
        auto const entries =
            Entries::from_checked_bytes(bytes, bytes.size() / sizeof(Addr), decoder);
        if (entries.is_some()) {
            return ustl::some(DynamicFunctionArrayView(entries.value()));
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_entries.size();
    }

    [[nodiscard]]
    auto at(usize index) const noexcept -> ustl::Option<Addr> {
        return m_entries.at(index);
    }

    [[nodiscard]]
    constexpr auto begin() const noexcept -> typename Entries::Iterator {
        return m_entries.begin();
    }

    [[nodiscard]]
    constexpr auto end() const noexcept -> typename Entries::Iterator {
        return m_entries.end();
    }

  private:
    constexpr explicit DynamicFunctionArrayView(Entries entries) noexcept : m_entries(entries) {}

    template <typename Codec>
    [[nodiscard]]
    static constexpr auto decoder_for(ByteOrder byte_order) noexcept
        -> decltype(&Codec::decode_little) {
        if (byte_order == ByteOrder::Little) {
            return &Codec::decode_little;
        }
        return &Codec::decode_big;
    }

    Entries m_entries;
};
} // namespace eftl::elf

#endif // EFTL_ELF_DYNAMIC_ARRAY_HPP
