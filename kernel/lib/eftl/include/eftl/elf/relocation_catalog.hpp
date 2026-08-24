/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_RELOC_ENTRYATION_CATALOG_HPP
#define EFTL_RELOC_ENTRYATION_CATALOG_HPP 1

#include <eftl/elf/details/types.hpp>
#include <eftl/elf/enums.hpp>

#include <array>

#include <ustl/option.hpp>
#include <ustl/views/span.hpp>

namespace eftl::elf {
// Architecture-specific relocation identity. The name is the stable EFTL
// catalog spelling; formulas and loader policy deliberately live elsewhere.
// NOLINTBEGIN(readability-identifier-naming)
struct RelocationInfo final {
    Arch machine{Arch::None};
    u32 type{0};
    char const *name{nullptr};
};
// NOLINTEND(readability-identifier-naming)

template <Arch Machine>
struct RelocationCatalog;

template <>
struct RelocationCatalog<Arch::I386> final {
    using Info = RelocationInfo;

    [[nodiscard]]
    static constexpr auto entries() noexcept -> ustl::views::Span<Info const> {
        return {K_ENTRIES.data(), K_ENTRIES.size()};
    }

    [[nodiscard]]
    static auto lookup(u32 type) noexcept -> ustl::Option<Info> {
        for (auto const entry: entries()) {
            if (entry.type == type) {
                return ustl::some(Info{entry});
            }
        }
        return ustl::NONE;
    }

  private:
    inline static constexpr auto K_ENTRIES = std::to_array<Info>({
        Info{Arch::I386, 0, "X86_NONE"},
        Info{Arch::I386, 1, "X86_32"},
        Info{Arch::I386, 2, "X86_PC32"},
        Info{Arch::I386, 3, "X86_GOT32"},
        Info{Arch::I386, 4, "X86_PLT32"},
        Info{Arch::I386, 5, "X86_COPY"},
        Info{Arch::I386, 6, "X86_GLOB_DAT"},
        Info{Arch::I386, 7, "X86_JUMP_SLOT"},
        Info{Arch::I386, 8, "X86_RELATIVE"},
        Info{Arch::I386, 9, "X86_GOTOFF"},
        Info{Arch::I386, 10, "X86_GOTPC"},
        Info{Arch::I386, 11, "X86_32PLT"},
        Info{Arch::I386, 14, "X86_TLS_TPOFF"},
        Info{Arch::I386, 15, "X86_TLS_IE"},
        Info{Arch::I386, 16, "X86_TLS_GOTIE"},
        Info{Arch::I386, 17, "X86_TLS_LE"},
        Info{Arch::I386, 18, "X86_TLS_GD"},
        Info{Arch::I386, 19, "X86_TLS_LDM"},
        Info{Arch::I386, 20, "X86_16"},
        Info{Arch::I386, 21, "X86_PC16"},
        Info{Arch::I386, 22, "X86_8"},
        Info{Arch::I386, 23, "X86_PC8"},
        Info{Arch::I386, 24, "X86_TLS_GD_32"},
        Info{Arch::I386, 25, "X86_TLS_GD_PUSH"},
        Info{Arch::I386, 26, "X86_TLS_GD_CALL"},
        Info{Arch::I386, 27, "X86_TLS_GD_POP"},
        Info{Arch::I386, 28, "X86_TLS_LDM_32"},
        Info{Arch::I386, 29, "X86_TLS_LDM_PUSH"},
        Info{Arch::I386, 30, "X86_TLS_LDM_CALL"},
        Info{Arch::I386, 31, "X86_TLS_LDM_POP"},
        Info{Arch::I386, 32, "X86_TLS_LDO_32"},
        Info{Arch::I386, 33, "X86_TLS_IE_32"},
        Info{Arch::I386, 34, "X86_TLS_LE_32"},
        Info{Arch::I386, 35, "X86_TLS_DTPMOD32"},
        Info{Arch::I386, 36, "X86_TLS_DTPOFF32"},
        Info{Arch::I386, 37, "X86_TLS_TPOFF32"},
        Info{Arch::I386, 38, "X86_SIZE32"},
        Info{Arch::I386, 39, "X86_TLS_GOTDESC"},
        Info{Arch::I386, 40, "X86_TLS_DESC_CALL"},
        Info{Arch::I386, 41, "X86_TLS_DESC"},
        Info{Arch::I386, 42, "X86_IRELATIVE"},
        Info{Arch::I386, 43, "X86_GOT32X"},
    });
};

template <>
struct RelocationCatalog<Arch::X86_64> final {
    using Info = RelocationInfo;

    [[nodiscard]]
    static constexpr auto entries() noexcept -> ustl::views::Span<Info const> {
        return {K_ENTRIES.data(), K_ENTRIES.size()};
    }

    [[nodiscard]]
    static auto lookup(u32 type) noexcept -> ustl::Option<Info> {
        for (auto const entry: entries()) {
            if (entry.type == type) {
                return ustl::some(Info{entry});
            }
        }
        return ustl::NONE;
    }

  private:
    inline static constexpr auto K_ENTRIES = std::to_array<Info>({
        Info{Arch::X86_64, 0, "X86_64_NONE"},
        Info{Arch::X86_64, 1, "X86_64_64"},
        Info{Arch::X86_64, 2, "X86_64_PC32"},
        Info{Arch::X86_64, 3, "X86_64_GOT32"},
        Info{Arch::X86_64, 4, "X86_64_PLT32"},
        Info{Arch::X86_64, 5, "X86_64_COPY"},
        Info{Arch::X86_64, 6, "X86_64_GLOB_DAT"},
        Info{Arch::X86_64, 7, "X86_64_JUMP_SLOT"},
        Info{Arch::X86_64, 8, "X86_64_RELATIVE"},
        Info{Arch::X86_64, 9, "X86_64_GOTPCREL"},
        Info{Arch::X86_64, 10, "X86_64_32"},
        Info{Arch::X86_64, 11, "X86_64_32S"},
        Info{Arch::X86_64, 12, "X86_64_16"},
        Info{Arch::X86_64, 13, "X86_64_PC16"},
        Info{Arch::X86_64, 14, "X86_64_8"},
        Info{Arch::X86_64, 15, "X86_64_PC8"},
        Info{Arch::X86_64, 16, "X86_64_DTPMOD64"},
        Info{Arch::X86_64, 17, "X86_64_DTPOFF64"},
        Info{Arch::X86_64, 18, "X86_64_TPOFF64"},
        Info{Arch::X86_64, 19, "X86_64_TLSGD"},
        Info{Arch::X86_64, 20, "X86_64_TLSLD"},
        Info{Arch::X86_64, 21, "X86_64_DTPOFF32"},
        Info{Arch::X86_64, 22, "X86_64_GOTTPOFF"},
        Info{Arch::X86_64, 23, "X86_64_TPOFF32"},
        Info{Arch::X86_64, 24, "X86_64_PC64"},
        Info{Arch::X86_64, 25, "X86_64_GOTOFF64"},
        Info{Arch::X86_64, 26, "X86_64_GOTPC32"},
        Info{Arch::X86_64, 27, "X86_64_GOT64"},
        Info{Arch::X86_64, 28, "X86_64_GOTPCREL64"},
        Info{Arch::X86_64, 29, "X86_64_GOTPC64"},
        Info{Arch::X86_64, 30, "X86_64_GOTPLT64"},
        Info{Arch::X86_64, 31, "X86_64_PLTOFF64"},
        Info{Arch::X86_64, 32, "X86_64_SIZE32"},
        Info{Arch::X86_64, 33, "X86_64_SIZE64"},
        Info{Arch::X86_64, 34, "X86_64_GOTPC32_TLSDESC"},
        Info{Arch::X86_64, 35, "X86_64_TLSDESC_CALL"},
        Info{Arch::X86_64, 36, "X86_64_TLSDESC"},
        Info{Arch::X86_64, 37, "X86_64_IRELATIVE"},
        Info{Arch::X86_64, 38, "X86_64_RELATIVE64"},
        Info{Arch::X86_64, 39, "X86_64_PC32_BND"},
        Info{Arch::X86_64, 40, "X86_64_PLT32_BND"},
        Info{Arch::X86_64, 41, "X86_64_GOTPCRELX"},
        Info{Arch::X86_64, 42, "X86_64_REX_GOTPCRELX"},
        Info{Arch::X86_64, 43, "X86_64_CODE_4_GOTPCRELX"},
        Info{Arch::X86_64, 44, "X86_64_CODE_4_GOTTPOFF"},
        Info{Arch::X86_64, 45, "X86_64_CODE_4_GOTPC32_TLSDESC"},
        Info{Arch::X86_64, 50, "X86_64_CODE_6_GOTTPOFF"},
    });
};
} // namespace eftl::elf

#endif // EFTL_RELOC_ENTRYATION_CATALOG_HPP
