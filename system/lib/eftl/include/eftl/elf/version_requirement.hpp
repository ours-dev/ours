/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_VERSION_REQUIREMENT_HPP
#define EFTL_ELF_VERSION_REQUIREMENT_HPP 1

#include <eftl/elf/section.hpp>
#include <eftl/elf/traits.hpp>
#include <eftl/elf/version_detail.hpp>

#include <limits>

#include <ustl/option.hpp>

namespace eftl::elf {
template <typename Traits>
class VersionRequirementTableView final {
  public:
    using Requirement = typename Traits::Verneed;
    using Auxiliary = typename Traits::Vernaux;
    using Version = typename Traits::Half;

    [[nodiscard]]
    static auto from(ByteRange entries, ByteRange strings,
                     ByteOrder byte_order = ByteOrder::Little) noexcept
        -> ustl::Option<VersionRequirementTableView> {
        auto const names = StringTableView::from(strings);
        auto const count = validated_count(entries, names, byte_order);
        if (names.is_some() && count.is_some()) {
            return ustl::some(
                VersionRequirementTableView(entries, names.value(), count.value(), byte_order));
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_count;
    }

    [[nodiscard]]
    auto requirement_at(usize index) const noexcept -> ustl::Option<Requirement> {
        auto const offset = requirement_offset(index);
        if (offset.is_none()) {
            return ustl::NONE;
        }
        return detail::read_version_record<Traits, Requirement>(m_entries, offset.value(),
                                                                m_byte_order);
    }

    [[nodiscard]]
    auto auxiliary_at(usize requirement_index, usize auxiliary_index) const noexcept
        -> ustl::Option<Auxiliary> {
        auto const requirement = requirement_at(requirement_index);
        auto const requirement_position = requirement_offset(requirement_index);
        if (requirement.is_none() || requirement_position.is_none() ||
            auxiliary_index >= requirement->vn_cnt) {
            return ustl::NONE;
        }

        auto offset =
            detail::version_relative_offset(requirement_position.value(), requirement->vn_aux,
                                            sizeof(Requirement), m_entries.size());
        if (offset.is_none()) {
            return ustl::NONE;
        }
        for (usize index = 0; index < auxiliary_index; ++index) {
            auto const auxiliary = detail::read_version_record<Traits, Auxiliary>(
                m_entries, offset.value(), m_byte_order);
            if (auxiliary.is_none()) {
                return ustl::NONE;
            }
            offset = detail::version_relative_offset(offset.value(), auxiliary->vna_next,
                                                     sizeof(Auxiliary), m_entries.size());
            if (offset.is_none()) {
                return ustl::NONE;
            }
        }
        return detail::read_version_record<Traits, Auxiliary>(m_entries, offset.value(),
                                                              m_byte_order);
    }

    [[nodiscard]]
    auto file_name(usize index) const noexcept -> ustl::Option<ByteRange> {
        auto const requirement = requirement_at(index);
        if (requirement.is_none()) {
            return ustl::NONE;
        }
        auto const name_offset = detail::version_to_usize(requirement->vn_file);
        if (name_offset.is_none()) {
            return ustl::NONE;
        }
        return m_names.string_at(name_offset.value());
    }

    [[nodiscard]]
    auto auxiliary_name(usize requirement_index, usize auxiliary_index) const noexcept
        -> ustl::Option<ByteRange> {
        auto const auxiliary = auxiliary_at(requirement_index, auxiliary_index);
        if (auxiliary.is_none()) {
            return ustl::NONE;
        }
        auto const name_offset = detail::version_to_usize(auxiliary->vna_name);
        if (name_offset.is_none()) {
            return ustl::NONE;
        }
        return m_names.string_at(name_offset.value());
    }

    [[nodiscard]]
    static constexpr auto version_index(Version version) noexcept -> Version {
        return static_cast<Version>(version & VERSION_INDEX_MASK);
    }

    [[nodiscard]]
    static constexpr auto is_hidden(Version version) noexcept -> bool {
        return (version & HIDDEN_MASK) != 0;
    }

  private:
    static constexpr Version VERSION_INDEX_MASK = static_cast<Version>(0x7fffU);
    static constexpr Version HIDDEN_MASK = static_cast<Version>(0x8000U);

    explicit constexpr VersionRequirementTableView(ByteRange entries, StringTableView names,
                                                   usize count, ByteOrder byte_order) noexcept
        : m_entries(entries), m_names(names), m_count(count), m_byte_order(byte_order) {}

    [[nodiscard]]
    static auto validate_auxiliaries(ByteRange entries, StringTableView const &names,
                                     usize requirement_offset, Requirement const &requirement,
                                     ByteOrder byte_order) noexcept -> ustl::Option<usize> {
        if (requirement.vn_cnt == 0) {
            return ustl::NONE;
        }
        auto offset = detail::version_relative_offset(requirement_offset, requirement.vn_aux,
                                                      sizeof(Requirement), entries.size());
        if (offset.is_none()) {
            return ustl::NONE;
        }

        for (usize index = 0; index < requirement.vn_cnt; ++index) {
            auto const auxiliary =
                detail::read_version_record<Traits, Auxiliary>(entries, offset.value(), byte_order);
            if (auxiliary.is_none()) {
                return ustl::NONE;
            }
            auto const name_offset = detail::version_to_usize(auxiliary->vna_name);
            if (name_offset.is_none() || names.string_at(name_offset.value()).is_none()) {
                return ustl::NONE;
            }

            if (index + 1 == requirement.vn_cnt) {
                if (auxiliary->vna_next != 0 ||
                    offset.value() > entries.size() - sizeof(Auxiliary)) {
                    return ustl::NONE;
                }
                return ustl::some(offset.value() + sizeof(Auxiliary));
            }

            offset = detail::version_relative_offset(offset.value(), auxiliary->vna_next,
                                                     sizeof(Auxiliary), entries.size());
            if (offset.is_none()) {
                return ustl::NONE;
            }
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    static auto validated_count(ByteRange entries, ustl::Option<StringTableView> const &names,
                                ByteOrder byte_order) noexcept -> ustl::Option<usize> {
        if (names.is_none() || (!entries.empty() && entries.data() == nullptr)) {
            return ustl::NONE;
        }
        if (entries.empty()) {
            return ustl::some(usize{0});
        }

        usize requirement_offset = 0;
        usize count = 0;
        while (true) {
            auto const requirement = detail::read_version_record<Traits, Requirement>(
                entries, requirement_offset, byte_order);
            if (requirement.is_none()) {
                return ustl::NONE;
            }
            auto const file_offset = detail::version_to_usize(requirement->vn_file);
            if (requirement->vn_version != 1 || file_offset.is_none() ||
                names->string_at(file_offset.value()).is_none()) {
                return ustl::NONE;
            }
            auto const auxiliary_end = validate_auxiliaries(
                entries, names.value(), requirement_offset, requirement.value(), byte_order);
            if (auxiliary_end.is_none() || count == std::numeric_limits<usize>::max()) {
                return ustl::NONE;
            }
            ++count;

            if (requirement->vn_next == 0) {
                return ustl::some(usize{count});
            }
            auto const next = detail::version_relative_offset(
                requirement_offset, requirement->vn_next, sizeof(Requirement), entries.size());
            if (next.is_none() || next.value() < auxiliary_end.value()) {
                return ustl::NONE;
            }
            requirement_offset = next.value();
        }
    }

    [[nodiscard]]
    auto requirement_offset(usize index) const noexcept -> ustl::Option<usize> {
        if (index >= m_count) {
            return ustl::NONE;
        }

        usize offset = 0;
        for (usize current = 0; current < index; ++current) {
            auto const requirement =
                detail::read_version_record<Traits, Requirement>(m_entries, offset, m_byte_order);
            if (requirement.is_none()) {
                return ustl::NONE;
            }
            auto const next = detail::version_relative_offset(
                offset, requirement->vn_next, sizeof(Requirement), m_entries.size());
            if (next.is_none()) {
                return ustl::NONE;
            }
            offset = next.value();
        }
        return ustl::some(usize{offset});
    }

    ByteRange m_entries{};
    StringTableView m_names;
    usize m_count{0};
    ByteOrder m_byte_order{ByteOrder::Little};
};
} // namespace eftl::elf

#endif // EFTL_ELF_VERSION_REQUIREMENT_HPP
