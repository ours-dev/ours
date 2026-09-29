/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_DYNAMIC_LINKAGE_HPP
#define EFTL_ELF_DYNAMIC_LINKAGE_HPP 1

#include <eftl/elf/details/dyn.hpp>
#include <eftl/elf/section.hpp>
#include <eftl/elf/table.hpp>
#include <eftl/elf/traits.hpp>

#include <limits>

#include <ustl/option.hpp>

namespace eftl::elf {
// Provides semantic access to dynamic-linkage facts without loader policy.
template <typename Traits>
class DynamicLinkageView final {
  public:
    using Dynamic = typename Traits::Dyn;
    using Dynamics = TableView<Dynamic>;

    [[nodiscard]]
    static auto from(Dynamics entries, StringTableView strings) noexcept
        -> ustl::Option<DynamicLinkageView> {
        auto needed_count = usize{0};
        for (auto const entry: entries) {
            auto const entry_tag = tag(entry);
            if (is_string_tag(entry_tag) && string_at(strings, value(entry)).is_none()) {
                return ustl::NONE;
            }
            if (entry_tag != DynTag::Needed) {
                continue;
            }
            if (needed_count == std::numeric_limits<usize>::max()) {
                return ustl::NONE;
            }
            ++needed_count;
        }
        if (has_duplicate(entries, DynTag::SoName) || has_duplicate(entries, DynTag::Rpath) ||
            has_duplicate(entries, DynTag::RunPath) || has_duplicate(entries, DynTag::Flags) ||
            has_duplicate(entries, DynTag::Flags1)) {
            return ustl::NONE;
        }
        return ustl::some(DynamicLinkageView(entries, strings, needed_count));
    }

    [[nodiscard]]
    constexpr auto needed_library_count() const noexcept -> usize {
        return m_needed_count;
    }

    [[nodiscard]]
    auto needed_library_at(usize index) const noexcept -> ustl::Option<ByteRange> {
        if (index >= m_needed_count) {
            return ustl::NONE;
        }

        auto current_index = usize{0};
        for (auto const entry: m_entries) {
            if (tag(entry) != DynTag::Needed) {
                continue;
            }
            if (current_index == index) {
                return string_at(m_strings, value(entry));
            }
            ++current_index;
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    auto soname() const noexcept -> ustl::Option<ByteRange> {
        return unique_string(DynTag::SoName);
    }

    [[nodiscard]]
    auto rpath() const noexcept -> ustl::Option<ByteRange> {
        return unique_string(DynTag::Rpath);
    }

    [[nodiscard]]
    auto runpath() const noexcept -> ustl::Option<ByteRange> {
        return unique_string(DynTag::RunPath);
    }

    [[nodiscard]]
    auto flags() const noexcept -> ustl::Option<u64> {
        return unique_value(DynTag::Flags);
    }

    [[nodiscard]]
    auto flags1() const noexcept -> ustl::Option<u64> {
        return unique_value(DynTag::Flags1);
    }

  private:
    constexpr DynamicLinkageView(Dynamics entries, StringTableView strings,
                                 usize needed_count) noexcept
        : m_entries(entries), m_strings(strings), m_needed_count(needed_count) {}

    [[nodiscard]]
    static constexpr auto tag(Dynamic const &entry) noexcept -> DynTag {
        return static_cast<DynTag>(static_cast<u64>(entry.d_tag));
    }

    [[nodiscard]]
    static constexpr auto value(Dynamic const &entry) noexcept -> u64 {
        return static_cast<u64>(entry.d_val);
    }

    [[nodiscard]]
    static constexpr auto is_string_tag(DynTag entry_tag) noexcept -> bool {
        return entry_tag == DynTag::Needed || entry_tag == DynTag::SoName ||
               entry_tag == DynTag::Rpath || entry_tag == DynTag::RunPath;
    }

    [[nodiscard]]
    static auto has_duplicate(Dynamics entries, DynTag wanted) noexcept -> bool {
        auto seen = false;
        for (auto const entry: entries) {
            if (tag(entry) != wanted) {
                continue;
            }
            if (seen) {
                return true;
            }
            seen = true;
        }
        return false;
    }

    [[nodiscard]]
    static auto string_at(StringTableView const &strings, u64 offset) noexcept
        -> ustl::Option<ByteRange> {
        if constexpr (sizeof(u64) > sizeof(usize)) {
            if (offset > static_cast<u64>(std::numeric_limits<usize>::max())) {
                return ustl::NONE;
            }
        }
        return strings.string_at(static_cast<usize>(offset));
    }

    [[nodiscard]]
    auto unique_string(DynTag wanted) const noexcept -> ustl::Option<ByteRange> {
        ustl::Option<ByteRange> result;
        for (auto const entry: m_entries) {
            if (tag(entry) != wanted) {
                continue;
            }
            if (result.is_some()) {
                return ustl::NONE;
            }
            result = string_at(m_strings, value(entry));
            if (result.is_none()) {
                return ustl::NONE;
            }
        }
        return result;
    }

    [[nodiscard]]
    auto unique_value(DynTag wanted) const noexcept -> ustl::Option<u64> {
        ustl::Option<u64> result;
        for (auto const entry: m_entries) {
            if (tag(entry) != wanted) {
                continue;
            }
            if (result.is_some()) {
                return ustl::NONE;
            }
            result = value(entry);
        }
        return result;
    }

    Dynamics m_entries;
    StringTableView m_strings;
    usize m_needed_count{0};
};
} // namespace eftl::elf

#endif // EFTL_ELF_DYNAMIC_LINKAGE_HPP
