/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_VERSION_BINDING_HPP
#define EFTL_ELF_VERSION_BINDING_HPP 1

#include <eftl/elf/version_definition.hpp>
#include <eftl/elf/version_requirement.hpp>
#include <eftl/elf/version_symbol.hpp>

#include <ustl/option.hpp>

namespace eftl::elf {
enum class VersionBindingKind {
    Local,
    Global,
    Definition,
    Requirement,
};

template <typename Traits>
struct VersionBinding final {
    using Version = typename Traits::Half;

    VersionBindingKind m_kind{VersionBindingKind::Local};
    Version m_index{0};
    bool m_hidden{false};
    ByteRange m_name{};
    ByteRange m_provider{};
};

template <typename Traits>
class VersionResolver final {
  public:
    using Version = typename Traits::Half;
    using Versions = VersionSymbolTableView<Traits>;
    using Definitions = VersionDefinitionTableView<Traits>;
    using Requirements = VersionRequirementTableView<Traits>;
    using Binding = VersionBinding<Traits>;

    [[nodiscard]]
    static auto resolve_base(Versions const &versions, usize symbol_index) noexcept
        -> ustl::Option<Binding> {
        auto const version = versions.version_at(symbol_index);
        if (version.is_none()) {
            return ustl::NONE;
        }
        auto const index = Versions::version_index(version.value());
        auto const hidden = Versions::is_hidden(version.value());
        if (index == LOCAL_INDEX) {
            return ustl::some(Binding{
                .m_kind = VersionBindingKind::Local,
                .m_index = index,
                .m_hidden = hidden,
            });
        }
        if (index == GLOBAL_INDEX) {
            return ustl::some(Binding{
                .m_kind = VersionBindingKind::Global,
                .m_index = index,
                .m_hidden = hidden,
            });
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    static auto resolve_definition(Versions const &versions, Definitions const &definitions,
                                   usize symbol_index) noexcept -> ustl::Option<Binding> {
        auto const version = version_at(versions, symbol_index);
        if (version.is_none()) {
            return ustl::NONE;
        }
        auto const base = resolve_base(versions, symbol_index);
        if (base.is_some()) {
            return base;
        }

        ustl::Option<Binding> resolved;
        for (usize definition_index = 0; definition_index < definitions.size();
             ++definition_index) {
            auto const definition = definitions.definition_at(definition_index);
            if (definition.is_none() || definition->vd_ndx != version->m_index) {
                continue;
            }
            auto const name = definitions.definition_name(definition_index);
            if (name.is_none() || resolved.is_some()) {
                return ustl::NONE;
            }
            resolved = Binding{
                .m_kind = VersionBindingKind::Definition,
                .m_index = version->m_index,
                .m_hidden = version->m_hidden,
                .m_name = name.value(),
            };
        }
        return resolved;
    }

    [[nodiscard]]
    static auto resolve_requirement(Versions const &versions, Requirements const &requirements,
                                    usize symbol_index) noexcept -> ustl::Option<Binding> {
        auto const version = version_at(versions, symbol_index);
        if (version.is_none()) {
            return ustl::NONE;
        }
        auto const base = resolve_base(versions, symbol_index);
        if (base.is_some()) {
            return base;
        }

        ustl::Option<Binding> resolved;
        for (usize requirement_index = 0; requirement_index < requirements.size();
             ++requirement_index) {
            auto const provider = requirements.file_name(requirement_index);
            auto const requirement = requirements.requirement_at(requirement_index);
            if (provider.is_none() || requirement.is_none()) {
                return ustl::NONE;
            }
            for (usize auxiliary_index = 0; auxiliary_index < requirement->vn_cnt;
                 ++auxiliary_index) {
                auto const auxiliary =
                    requirements.auxiliary_at(requirement_index, auxiliary_index);
                if (auxiliary.is_none() ||
                    Requirements::version_index(auxiliary->vna_other) != version->m_index) {
                    continue;
                }
                auto const name = requirements.auxiliary_name(requirement_index, auxiliary_index);
                if (name.is_none() || resolved.is_some()) {
                    return ustl::NONE;
                }
                resolved = Binding{
                    .m_kind = VersionBindingKind::Requirement,
                    .m_index = version->m_index,
                    .m_hidden = version->m_hidden,
                    .m_name = name.value(),
                    .m_provider = provider.value(),
                };
            }
        }
        return resolved;
    }

  private:
    struct SelectedVersion final {
        Version m_index{0};
        bool m_hidden{false};
    };

    static constexpr Version LOCAL_INDEX = 0;
    static constexpr Version GLOBAL_INDEX = 1;

    [[nodiscard]]
    static auto version_at(Versions const &versions, usize symbol_index) noexcept
        -> ustl::Option<SelectedVersion> {
        auto const version = versions.version_at(symbol_index);
        if (version.is_none()) {
            return ustl::NONE;
        }
        return ustl::some(SelectedVersion{
            .m_index = Versions::version_index(version.value()),
            .m_hidden = Versions::is_hidden(version.value()),
        });
    }
};
} // namespace eftl::elf

#endif // EFTL_ELF_VERSION_BINDING_HPP
