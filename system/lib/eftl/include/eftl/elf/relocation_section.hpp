/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_RELOC_ENTRYATION_SECTION_HPP
#define EFTL_RELOC_ENTRYATION_SECTION_HPP 1

#include <eftl/elf/encoding.hpp>
#include <eftl/elf/table.hpp>
#include <eftl/elf/traits.hpp>

#include <ustl/option.hpp>

namespace eftl::elf {
template <typename Traits, typename Entry>
class RelocationTableView final {
  public:
    using Relocation = Entry;
    using Relocations = TableView<Relocation>;

    [[nodiscard]]
    static auto from(ByteRange const &entries, ByteOrder byte_order = ByteOrder::Little) noexcept
        -> ustl::Option<RelocationTableView> {
        if (entries.size() % sizeof(Relocation) != 0) {
            return ustl::NONE;
        }
        auto decoder = &RecordCodec<Traits, Relocation>::decode_little;
        if (byte_order != ByteOrder::Little) {
            decoder = &RecordCodec<Traits, Relocation>::decode_big;
        }
        auto const relocations =
            Relocations::from_checked_bytes(entries, entries.size() / sizeof(Relocation), decoder);
        if (relocations.is_some()) {
            return ustl::some(RelocationTableView(relocations.value()));
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_relocations.size();
    }

    [[nodiscard]]
    auto relocation_at(usize index) const noexcept -> ustl::Option<Relocation> {
        return m_relocations.at(index);
    }

    [[nodiscard]]
    static constexpr auto type(Relocation const &relocation) noexcept -> u32 {
        return Traits::relocation_type(relocation.r_info);
    }

    [[nodiscard]]
    static constexpr auto symbol_index(Relocation const &relocation) noexcept -> u32 {
        return Traits::relocation_symbol(relocation.r_info);
    }

  private:
    explicit constexpr RelocationTableView(Relocations relocations) noexcept
        : m_relocations(relocations) {}

    Relocations m_relocations;
};
} // namespace eftl::elf

#endif // EFTL_RELOC_ENTRYATION_SECTION_HPP
