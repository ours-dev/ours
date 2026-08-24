/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_VERSION_SYMBOL_HPP
#define EFTL_ELF_VERSION_SYMBOL_HPP 1

#include <eftl/elf/encoding.hpp>
#include <eftl/elf/table.hpp>

#include <ustl/option.hpp>

namespace eftl::elf {
template <typename Traits>
class VersionSymbolTableView final {
  public:
    using Version = typename Traits::Half;
    using Versions = TableView<Version>;

    [[nodiscard]]
    static auto from(ByteRange entries, ByteOrder byte_order = ByteOrder::Little) noexcept
        -> ustl::Option<VersionSymbolTableView> {
        if (entries.size() % sizeof(Version) != 0) {
            return ustl::NONE;
        }
        auto decoder = &ScalarCodec<Version>::decode_little;
        if (byte_order != ByteOrder::Little) {
            decoder = &ScalarCodec<Version>::decode_big;
        }
        auto const versions =
            Versions::from_checked_bytes(entries, entries.size() / sizeof(Version), decoder);
        if (versions.is_some()) {
            return ustl::some(VersionSymbolTableView(versions.value()));
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_versions.size();
    }

    [[nodiscard]]
    auto version_at(usize index) const noexcept -> ustl::Option<Version> {
        return m_versions.at(index);
    }

    [[nodiscard]]
    static constexpr auto version_index(Version version) noexcept -> Version {
        return static_cast<Version>(version & K_VERSION_INDEX_MASK);
    }

    [[nodiscard]]
    static constexpr auto is_hidden(Version version) noexcept -> bool {
        return (version & K_HIDDEN_MASK) != 0;
    }

  private:
    static constexpr Version K_VERSION_INDEX_MASK = static_cast<Version>(0x7fffU);
    static constexpr Version K_HIDDEN_MASK = static_cast<Version>(0x8000U);

    explicit constexpr VersionSymbolTableView(Versions versions) noexcept : m_versions(versions) {}

    Versions m_versions;
};
} // namespace eftl::elf

#endif // EFTL_ELF_VERSION_SYMBOL_HPP
