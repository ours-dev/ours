/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_DYNAMIC_SECTION_HPP
#define EFTL_ELF_DYNAMIC_SECTION_HPP 1

#include <eftl/elf/details/dyn.hpp>
#include <eftl/elf/dynamic_linkage.hpp>
#include <eftl/elf/encoding.hpp>
#include <eftl/elf/section.hpp>
#include <eftl/elf/table.hpp>
#include <eftl/elf/traits.hpp>

#include <limits>

#include <ustl/option.hpp>

namespace eftl::elf {
template <typename Traits>
class DynamicTableView final {
  public:
    using Dynamic = typename Traits::Dyn;
    using Dynamics = TableView<Dynamic>;

    [[nodiscard]]
    static auto from(ByteRange entries, ByteRange strings,
                     ByteOrder byte_order = ByteOrder::Little) noexcept
        -> ustl::Option<DynamicTableView> {
        if (entries.size() % sizeof(Dynamic) != 0) {
            return ustl::NONE;
        }
        auto const decoder = decoder_for<RecordCodec<Traits, Dynamic>>(byte_order);
        auto const dynamics =
            Dynamics::from_checked_bytes(entries, entries.size() / sizeof(Dynamic), decoder);
        auto const names = StringTableView::from(strings);
        if (dynamics.is_none() || names.is_none()) {
            return ustl::NONE;
        }

        auto const terminator = terminator_index(dynamics.value());
        if (terminator.is_some()) {
            return ustl::some(
                DynamicTableView(dynamics.value(), names.value(), terminator.value()));
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_count;
    }

    [[nodiscard]]
    auto entry_at(usize index) const noexcept -> ustl::Option<Dynamic> {
        if (index < m_count) {
            return m_dynamics.at(index);
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    auto string_at(Dynamic const &entry) const noexcept -> ustl::Option<ByteRange> {
        auto const string_index = value(entry);
        if constexpr (sizeof(u64) > sizeof(usize)) {
            if (string_index > static_cast<u64>(std::numeric_limits<usize>::max())) {
                return ustl::NONE;
            }
        }
        return m_names.string_at(static_cast<usize>(string_index));
    }

    [[nodiscard]]
    auto linkage() const noexcept -> ustl::Option<DynamicLinkageView<Traits>> {
        auto const entries = m_dynamics.first(m_count);
        if (entries.is_some()) {
            return DynamicLinkageView<Traits>::from(entries.value(), m_names);
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    static constexpr auto tag(Dynamic const &entry) noexcept -> DynTag {
        return static_cast<DynTag>(static_cast<u64>(entry.d_tag));
    }

    [[nodiscard]]
    static constexpr auto value(Dynamic const &entry) noexcept -> u64 {
        return static_cast<u64>(entry.d_val);
    }

  private:
    constexpr DynamicTableView(Dynamics dynamics, StringTableView names, usize count) noexcept
        : m_dynamics(dynamics), m_names(names), m_count(count) {}

    template <typename Codec>
    [[nodiscard]]
    static constexpr auto decoder_for(ByteOrder byte_order) noexcept
        -> decltype(&Codec::decode_little) {
        if (byte_order == ByteOrder::Little) {
            return &Codec::decode_little;
        }
        return &Codec::decode_big;
    }

    [[nodiscard]]
    static auto terminator_index(Dynamics const &dynamics) noexcept -> ustl::Option<usize> {
        for (usize index = 0; index < dynamics.size(); ++index) {
            if (tag(dynamics[index]) != DynTag::DtNull) {
                continue;
            }
            for (usize remaining = index + 1; remaining < dynamics.size(); ++remaining) {
                if (tag(dynamics[remaining]) != DynTag::DtNull) {
                    return ustl::NONE;
                }
            }
            return ustl::some(static_cast<usize>(index));
        }
        return ustl::NONE;
    }

    Dynamics m_dynamics;
    StringTableView m_names;
    usize m_count{0};
};
} // namespace eftl::elf

#endif // EFTL_ELF_DYNAMIC_SECTION_HPP
