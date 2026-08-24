/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_VERSION_DEFINITION_HPP
#define EFTL_ELF_VERSION_DEFINITION_HPP 1

#include <eftl/elf/section.hpp>
#include <eftl/elf/traits.hpp>
#include <eftl/elf/version_detail.hpp>

#include <limits>

#include <ustl/option.hpp>

namespace eftl::elf {
template <typename Traits>
class VersionDefinitionTableView final {
  public:
    using Definition = typename Traits::Verdef;
    using Auxiliary = typename Traits::Verdaux;

    [[nodiscard]]
    static auto from(ByteRange entries, ByteRange strings,
                     ByteOrder byte_order = ByteOrder::Little) noexcept
        -> ustl::Option<VersionDefinitionTableView> {
        auto const names = StringTableView::from(strings);
        auto const count = validated_count(entries, names, byte_order);
        if (names.is_some() && count.is_some()) {
            return ustl::some(
                VersionDefinitionTableView(entries, names.value(), count.value(), byte_order));
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_count;
    }

    [[nodiscard]]
    auto definition_at(usize index) const noexcept -> ustl::Option<Definition> {
        auto const offset = definition_offset(index);
        if (offset.is_none()) {
            return ustl::NONE;
        }
        return detail::read_version_record<Traits, Definition>(m_entries, offset.value(),
                                                               m_byte_order);
    }

    [[nodiscard]]
    auto auxiliary_at(usize definition_index, usize auxiliary_index) const noexcept
        -> ustl::Option<Auxiliary> {
        auto const definition = definition_at(definition_index);
        auto const definition_position = definition_offset(definition_index);
        if (definition.is_none() || definition_position.is_none() ||
            auxiliary_index >= definition->vd_cnt) {
            return ustl::NONE;
        }

        auto offset = detail::version_relative_offset(
            definition_position.value(), definition->vd_aux, sizeof(Definition), m_entries.size());
        if (offset.is_none()) {
            return ustl::NONE;
        }
        for (usize index = 0; index < auxiliary_index; ++index) {
            auto const auxiliary = detail::read_version_record<Traits, Auxiliary>(
                m_entries, offset.value(), m_byte_order);
            if (auxiliary.is_none()) {
                return ustl::NONE;
            }
            offset = detail::version_relative_offset(offset.value(), auxiliary->vda_next,
                                                     sizeof(Auxiliary), m_entries.size());
            if (offset.is_none()) {
                return ustl::NONE;
            }
        }
        return detail::read_version_record<Traits, Auxiliary>(m_entries, offset.value(),
                                                              m_byte_order);
    }

    [[nodiscard]]
    auto definition_name(usize index) const noexcept -> ustl::Option<ByteRange> {
        return auxiliary_name(index, 0);
    }

    [[nodiscard]]
    auto auxiliary_name(usize definition_index, usize auxiliary_index) const noexcept
        -> ustl::Option<ByteRange> {
        auto const auxiliary = auxiliary_at(definition_index, auxiliary_index);
        if (auxiliary.is_none()) {
            return ustl::NONE;
        }
        auto const name_offset = detail::version_to_usize(auxiliary->vda_name);
        if (name_offset.is_none()) {
            return ustl::NONE;
        }
        return m_names.string_at(name_offset.value());
    }

  private:
    explicit constexpr VersionDefinitionTableView(ByteRange entries, StringTableView names,
                                                  usize count, ByteOrder byte_order) noexcept
        : m_entries(entries), m_names(names), m_count(count), m_byte_order(byte_order) {}

    [[nodiscard]]
    static auto validate_auxiliaries(ByteRange entries, StringTableView const &names,
                                     usize definition_offset, Definition const &definition,
                                     ByteOrder byte_order) noexcept -> ustl::Option<usize> {
        if (definition.vd_cnt == 0) {
            return ustl::NONE;
        }
        auto offset = detail::version_relative_offset(definition_offset, definition.vd_aux,
                                                      sizeof(Definition), entries.size());
        if (offset.is_none()) {
            return ustl::NONE;
        }

        for (usize index = 0; index < definition.vd_cnt; ++index) {
            auto const auxiliary =
                detail::read_version_record<Traits, Auxiliary>(entries, offset.value(), byte_order);
            if (auxiliary.is_none()) {
                return ustl::NONE;
            }
            auto const name_offset = detail::version_to_usize(auxiliary->vda_name);
            if (name_offset.is_none() || names.string_at(name_offset.value()).is_none()) {
                return ustl::NONE;
            }

            if (index + 1 == definition.vd_cnt) {
                if (auxiliary->vda_next != 0 ||
                    offset.value() > entries.size() - sizeof(Auxiliary)) {
                    return ustl::NONE;
                }
                return ustl::some(offset.value() + sizeof(Auxiliary));
            }

            offset = detail::version_relative_offset(offset.value(), auxiliary->vda_next,
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

        usize definition_offset = 0;
        usize count = 0;
        while (true) {
            auto const definition = detail::read_version_record<Traits, Definition>(
                entries, definition_offset, byte_order);
            if (definition.is_none() || definition->vd_version != 1) {
                return ustl::NONE;
            }
            auto const auxiliary_end = validate_auxiliaries(
                entries, names.value(), definition_offset, definition.value(), byte_order);
            if (auxiliary_end.is_none() || count == std::numeric_limits<usize>::max()) {
                return ustl::NONE;
            }
            ++count;

            if (definition->vd_next == 0) {
                return ustl::some(usize{count});
            }
            auto const next = detail::version_relative_offset(
                definition_offset, definition->vd_next, sizeof(Definition), entries.size());
            if (next.is_none() || next.value() < auxiliary_end.value()) {
                return ustl::NONE;
            }
            definition_offset = next.value();
        }
    }

    [[nodiscard]]
    auto definition_offset(usize index) const noexcept -> ustl::Option<usize> {
        if (index >= m_count) {
            return ustl::NONE;
        }

        usize offset = 0;
        for (usize current = 0; current < index; ++current) {
            auto const definition =
                detail::read_version_record<Traits, Definition>(m_entries, offset, m_byte_order);
            if (definition.is_none()) {
                return ustl::NONE;
            }
            auto const next = detail::version_relative_offset(offset, definition->vd_next,
                                                              sizeof(Definition), m_entries.size());
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

#endif // EFTL_ELF_VERSION_DEFINITION_HPP
