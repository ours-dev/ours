/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef EFTL_ELF_VIEWS_HPP
#define EFTL_ELF_VIEWS_HPP 1

#include <eftl/elf/dynamic.hpp>
#include <eftl/elf/enums.hpp>
#include <eftl/elf/link.hpp>
#include <eftl/elf/parser.hpp>
#include <eftl/elf/relocations.hpp>
#include <eftl/elf/segment.hpp>
#include <eftl/elf/traits.hpp>

#include <limits>
#include <utility>

#include <ustl/io/direct_memory.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

namespace eftl::elf {
template <typename Traits = TraitsNative>
class View final {
    using Self = View;

  public:
    using Addr = typename Traits::Addr;
    using Off = typename Traits::Off;
    using Half = typename Traits::Half;
    using Word = typename Traits::Word;
    using Sword = typename Traits::Sword;

    using Header = typename Traits::Header;
    using Segment = typename Traits::Segment;
    using Section = typename Traits::Section;
    using Symbol = typename Traits::Symbol;
    using Auxv = typename Traits::Auxv;
    using Dyn = typename Traits::Dyn;
    using Rel = typename Traits::Rel;
    using Rela = typename Traits::Rela;
    using Verneed = typename Traits::Verneed;
    using Verdef = typename Traits::Verdef;
    using Vernaux = typename Traits::Vernaux;
    using Prstatus = typename Traits::Prstatus;
    using FileEntry = typename Traits::FileEntry;

    using Segments = TableView<Segment>;
    using Sections = TableView<Section>;
    using Symbols = ustl::views::Span<Symbol const>;
    using Dynamics = TableView<Dyn>;
    using RelTable = ustl::views::Span<Rel const>;
    using RelaTable = ustl::views::Span<Rela const>;

    View() = default;

    View(Self const &) = default;

    View(Self &&) = default;

    ~View() = default;

    [[nodiscard]]
    static auto from_raw(void const *raw, usize size_bytes) -> ustl::Result<View, ParseError> {
        auto const structural_result = Parser::parse_as<Traits>(raw, size_bytes);
        if (structural_result.is_err()) {
            return ustl::err(structural_result.unwrap_err());
        }
        auto const native_image = structural_result->native_image();
        if (native_image.is_none()) {
            return ustl::err(ParseError::InvalidSegment);
        }

        Self view;
        view.m_binary.reset(raw, size_bytes);
        if (auto result = view.init(native_image.value())) {
            return ustl::err(result.value());
        }

        return ustl::ok(std::move(view));
    }

    template <typename Memory, typename IsRelative>
    [[nodiscard]]
    /// Apply architecture-approved relative relocations. Symbolic relocations are rejected.
    auto apply_relocation(Memory &&memory, Addr bias, IsRelative &&is_relative) const -> bool {
        if (m_dynamics.empty()) {
            return true;
        }
        Relocations<Traits> relocations;
        auto dynamics = m_dynamics;
        if (!decode_dynamic(memory, dynamics, RelocationObserver<Traits>(relocations))) {
            return false;
        }

        return relocate_relative(memory, relocations, bias, std::forward<IsRelative>(is_relative));
    }

    [[nodiscard]]
    auto virt_addr_to_offset(Addr addr) const noexcept -> ustl::Option<usize> {
        if (addr < m_va_minimal || addr >= m_va_maximal) {
            return ustl::NONE;
        }
        auto const offset = addr - m_va_minimal;
        if (offset > std::numeric_limits<usize>::max()) {
            return ustl::NONE;
        }
        return ustl::some(static_cast<usize>(offset));
    }

    [[nodiscard]]
    auto segments() const noexcept -> Segments {
        return m_segments;
    }

    [[nodiscard]]
    auto sections() const noexcept -> Sections {
        return m_sections;
    }

    [[nodiscard]]
    auto dynamics() const noexcept -> Dynamics {
        return m_dynamics;
    }

    [[nodiscard]]
    auto entry_point() const noexcept -> Addr {
        return m_header.e_entry;
    }

    [[nodiscard]]
    auto virtual_size() const noexcept -> usize {
        if (m_va_minimal == std::numeric_limits<Addr>::max() || m_va_maximal < m_va_minimal) {
            return 0;
        }
        return static_cast<usize>(m_va_maximal - m_va_minimal);
    }

    [[nodiscard]]
    auto virtual_base() const noexcept -> Addr {
        return m_va_minimal;
    }

    [[nodiscard]]
    auto machine() const noexcept -> Arch {
        return static_cast<Arch>(m_header.e_machine);
    }

    [[nodiscard]]
    auto binary() const noexcept -> ustl::io::DirectMemory const & {
        return m_binary;
    }

    [[nodiscard]]
    auto data() const noexcept -> u8 const * {
        return m_binary.address();
    }

    auto operator=(Self &&) -> Self & = default;
    auto operator=(Self const &) -> Self & = default;

  private:
    auto init(BasicImage<Traits> const &native_image) -> ustl::Option<ParseError> {
        auto const &structural = native_image.elf_file();
        m_header = structural.elf_header();
        m_segments = structural.segments();
        m_sections = structural.sections();

        if (m_header.e_phentsize != sizeof(Segment)) {
            return ustl::some(ParseError::InvalidHeader);
        }
        if (m_segments.empty()) {
            return ustl::some(ParseError::InvalidSegment);
        }

        bool found_load = false;
        bool entry_mapped = false;
        bool found_dynamic = false;
        m_va_minimal = std::numeric_limits<Addr>::max();
        m_va_maximal = 0;
        for (auto const segment: m_segments) {
            auto type = segment.type();
            if (type == SegType::Load) {
                found_load = true;
                auto const begin = segment.p_vaddr;
                auto const size = segment.p_memsz;
                if (begin > std::numeric_limits<Addr>::max() - size) {
                    return ustl::some(ParseError::InvalidSegment);
                }
                auto const end = begin + size;
                entry_mapped |= m_header.e_entry >= begin && m_header.e_entry - begin < size;
                if (begin < m_va_minimal) {
                    m_va_minimal = begin;
                }
                if (end > m_va_maximal) {
                    m_va_maximal = end;
                }
            } else if (type == SegType::Dynamic) {
                if (found_dynamic) {
                    return ustl::some(ParseError::InvalidSegment);
                }
                found_dynamic = true;
            }
        }
        if (!found_load || !entry_mapped) {
            return ustl::some(ParseError::InvalidSegment);
        }

        if (m_va_minimal == std::numeric_limits<Addr>::max() || m_va_maximal < m_va_minimal) {
            return ustl::some(ParseError::InvalidSegment);
        }
        if (m_va_maximal - m_va_minimal > std::numeric_limits<usize>::max()) {
            return ustl::some(ParseError::InvalidSegment);
        }

        if (found_dynamic) {
            auto const dynamic_image = structural.dynamic_image();
            if (dynamic_image.is_none()) {
                return ustl::some(ParseError::InvalidSegment);
            }
            m_dynamics = dynamic_image->entries();
        } else {
            m_dynamics = {};
        }

        return ustl::NONE;
    }

    ustl::io::DirectMemory m_binary;
    Header m_header{};
    Segments m_segments;
    Sections m_sections;
    Dynamics m_dynamics;
    Addr m_va_maximal{};
    Addr m_va_minimal{std::numeric_limits<Addr>::max()};
};

} // namespace eftl::elf

#endif // #ifndef EFTL_ELF_VIEWS_HPP
