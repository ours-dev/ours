/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_BINARY_HPP
#define EFTL_ELF_BINARY_HPP 1

#include <eftl/binary.hpp>
#include <eftl/binary_stream.hpp>
#include <eftl/elf/compression.hpp>
#include <eftl/elf/dynamic_image.hpp>
#include <eftl/elf/dynamic_section.hpp>
#include <eftl/elf/encoding.hpp>
#include <eftl/elf/gnu_hash.hpp>
#include <eftl/elf/gnu_property.hpp>
#include <eftl/elf/gnu_stack.hpp>
#include <eftl/elf/group.hpp>
#include <eftl/elf/note.hpp>
#include <eftl/elf/relocation_section.hpp>
#include <eftl/elf/relro.hpp>
#include <eftl/elf/section.hpp>
#include <eftl/elf/segment.hpp>
#include <eftl/elf/symbol.hpp>
#include <eftl/elf/sysv_hash.hpp>
#include <eftl/elf/table.hpp>
#include <eftl/elf/tls.hpp>
#include <eftl/elf/traits.hpp>
#include <eftl/elf/version_binding.hpp>
#include <eftl/elf/version_definition.hpp>
#include <eftl/elf/version_requirement.hpp>
#include <eftl/elf/version_symbol.hpp>

#include <limits>
#include <type_traits>
#include <utility>

#include <cstring>

#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <ustl/variant.hpp>

namespace eftl::elf {
enum class ParseError {
    NoError,
    InternalError,
    InvalidHeader,
    InvalidSegment,
    InvalidSection,
    InvalidSymbol,
    InvalidRelocation,
    InvalidNote,
    InvalidDynamic,
    InvalidVersion,
    InvalidHash,
    UnsupportedByteOrder,
    NotLoadable,
};

inline constexpr u16 PN_XNUM = 0xffff;
inline constexpr u16 SHN_LORESERVE = 0xff00;
inline constexpr u16 SHN_XINDEX = 0xffff;

[[nodiscard]]
constexpr auto to_string(ParseError parse_error) noexcept -> char const * {
    switch (parse_error) {
        case ParseError::NoError:
            return "No error";
        case ParseError::InvalidHeader:
            return "Invalid header";
        case ParseError::InvalidSegment:
            return "Invalid segment";
        case ParseError::InvalidSection:
            return "Invalid section";
        case ParseError::InvalidSymbol:
            return "Invalid symbol";
        case ParseError::InvalidRelocation:
            return "Invalid relocation";
        case ParseError::InvalidNote:
            return "Invalid note";
        case ParseError::InvalidDynamic:
            return "Invalid dynamic section";
        case ParseError::InvalidVersion:
            return "Invalid version section";
        case ParseError::InvalidHash:
            return "Invalid hash section";
        case ParseError::InternalError:
            return "Internal error";
        case ParseError::UnsupportedByteOrder:
            return "Unsupported byte order";
        case ParseError::NotLoadable:
            return "ELF is not a loadable image (expected ET_EXEC or ET_DYN)";
    }
    return "Unknown ELF parse error";
}

template <typename Traits>
class FileImpl;
template <typename Traits>
class BasicImage;

template <typename Traits>
class FileImpl final {
  public:
    using Header = typename Traits::Header;
    using Segment = typename Traits::Segment;
    using Section = typename Traits::Section;
    using Symbol = typename Traits::Symbol;
    using Dyn = typename Traits::Dyn;
    using Rel = typename Traits::Rel;
    using Rela = typename Traits::Rela;
    using Segments = TableView<Segment>;
    using Sections = TableView<Section>;

    [[nodiscard]]
    static auto from_raw(void const *raw, usize size) -> ustl::Result<FileImpl, ParseError> {
        auto const stream_result = BinaryStream::from(raw, size);
        if (stream_result.is_err()) {
            return ustl::err(ParseError::InvalidHeader);
        }

        FileImpl file(stream_result.unwrap());
        auto const parse_error = file.initialize();
        if (parse_error.is_some()) {
            return ustl::err(parse_error.value());
        }
        return ustl::ok(std::move(file));
    }

    [[nodiscard]]
    auto elf_header() const noexcept -> Header const & {
        return m_header;
    }

    [[nodiscard]]
    auto byte_order() const noexcept -> ByteOrder {
        return m_byte_order;
    }

    [[nodiscard]]
    auto segments() const noexcept -> Segments {
        return m_segments;
    }

    [[nodiscard]]
    auto sections() const noexcept -> Sections {
        return m_sections;
    }

    /// Returns the sole validated PT_INTERP pathname without its terminating NUL.
    [[nodiscard]]
    auto interpreter_path() const noexcept -> ustl::Option<ByteRange> {
        ustl::Option<ByteRange> path;
        for (auto const segment: m_segments) {
            if (segment.type() != SegType::Interp) {
                continue;
            }
            auto const candidate = interpreter_path_for(segment);
            if (path.is_some() || candidate.is_none()) {
                return ustl::NONE;
            }
            path = candidate.value();
        }
        return path;
    }

    /// Returns the sole validated PT_TLS template when the ELF defines one.
    [[nodiscard]]
    auto tls_template() const noexcept -> ustl::Option<TlsTemplate<Traits>> {
        ustl::Option<TlsTemplate<Traits>> tls;
        for (auto const segment: m_segments) {
            if (segment.type() != SegType::Tls) {
                continue;
            }
            auto const candidate = tls_template_for(segment);
            if (tls.is_some() || candidate.is_none()) {
                return ustl::NONE;
            }
            tls = candidate.value();
        }
        return tls;
    }

    /// Returns the sole PT_GNU_STACK request when the ELF defines one.
    [[nodiscard]]
    auto gnu_stack() const noexcept -> ustl::Option<GnuStack<Traits>> {
        ustl::Option<GnuStack<Traits>> stack;
        for (auto const segment: m_segments) {
            if (segment.type() != SegType::GnuStack) {
                continue;
            }
            auto const candidate = GnuStack<Traits>::from(segment);
            if (stack.is_some() || candidate.is_none()) {
                return ustl::NONE;
            }
            stack = candidate.value();
        }
        return stack;
    }

    /// Returns every validated PT_GNU_RELRO virtual range.
    [[nodiscard]]
    auto relro_regions() const noexcept -> RelroRegions<Traits> {
        return m_relro_regions;
    }

    /// Returns file-backed section bytes borrowed from the input ELF image.
    /// SHT_NOBITS has no file representation and returns NONE.
    [[nodiscard]]
    auto section_data(usize index) const noexcept -> ustl::Option<ByteRange> {
        auto const section = m_sections.at(index);
        if (section.is_none()) {
            return ustl::NONE;
        }
        return section_bytes(section.value());
    }

    /// Returns a section name borrowed from the validated section-name string table.
    [[nodiscard]]
    auto section_name(usize index) const noexcept -> ustl::Option<ByteRange> {
        if (m_section_names.is_none()) {
            return ustl::NONE;
        }
        auto const section = m_sections.at(index);
        if (section.is_none()) {
            return ustl::NONE;
        }
        auto const name_offset = to_usize(section->sh_name);
        if (name_offset.is_none()) {
            return ustl::NONE;
        }
        return m_section_names->string_at(name_offset.value());
    }

    [[nodiscard]]
    auto symbol_table(usize section_index) const noexcept -> ustl::Option<SymbolTableView<Traits>> {
        auto const section = m_sections.at(section_index);
        if (section.is_none() ||
            (static_cast<SectionType>(section->sh_type) != SectionType::Symtab &&
             static_cast<SectionType>(section->sh_type) != SectionType::Dynsym)) {
            return ustl::NONE;
        }
        auto const string_table = m_sections.at(static_cast<usize>(section->sh_link));
        auto const entries = section_bytes(section.value());
        ustl::Option<ByteRange> strings;
        if (string_table.is_some()) {
            strings = section_bytes(string_table.value());
        } else {
            strings = ustl::NONE;
        }
        auto const extended_indices = symbol_section_indices(section_index);
        if (entries.is_none() || strings.is_none()) {
            return ustl::NONE;
        }
        return SymbolTableView<Traits>::from(entries.value(), strings.value(), extended_indices,
                                             m_byte_order);
    }

    /// Returns a validated SHT_GROUP record and its signature-symbol relationship.
    [[nodiscard]]
    auto section_group(usize section_index) const noexcept -> ustl::Option<SectionGroupView> {
        auto const section = m_sections.at(section_index);
        if (section.is_none() || static_cast<SectionType>(section->sh_type) != SectionType::Group ||
            section->sh_entsize != sizeof(u32) || section->sh_addralign != alignof(u32)) {
            return ustl::NONE;
        }
        auto const symbol_section_index = to_usize(section->sh_link);
        auto const signature_symbol_index = to_usize(section->sh_info);
        ustl::Option<SymbolTableView<Traits>> symbols;
        if (symbol_section_index.is_some()) {
            symbols = symbol_table(symbol_section_index.value());
        } else {
            symbols = ustl::NONE;
        }
        auto const entries = section_bytes(section.value());
        if (signature_symbol_index.is_none() || signature_symbol_index.value() == 0 ||
            symbols.is_none() || symbols->symbol_at(signature_symbol_index.value()).is_none() ||
            entries.is_none()) {
            return ustl::NONE;
        }
        return SectionGroupView::from(entries.value(), signature_symbol_index.value(),
                                      m_sections.size(), section_index, m_byte_order);
    }

    /// Returns metadata and encoded bytes for an SHF_COMPRESSED section.
    [[nodiscard]]
    auto compressed_section(usize section_index) const noexcept
        -> ustl::Option<SectionCompressionView<Traits>> {
        constexpr auto COMPRESSED_FLAG = static_cast<u64>(SectionFlags::Compressed);
        auto const section = m_sections.at(section_index);
        if (section.is_none() ||
            static_cast<SectionType>(section->sh_type) == SectionType::Nobits ||
            (static_cast<u64>(section->sh_flags) & COMPRESSED_FLAG) == 0) {
            return ustl::NONE;
        }
        auto const bytes = section_bytes(section.value());
        if (bytes.is_none()) {
            return ustl::NONE;
        }
        return SectionCompressionView<Traits>::from(bytes.value(), m_byte_order);
    }

    [[nodiscard]]
    auto version_symbol_table(usize section_index) const noexcept
        -> ustl::Option<VersionSymbolTableView<Traits>> {
        auto const section = m_sections.at(section_index);
        if (section.is_none() ||
            static_cast<SectionType>(section->sh_type) != SectionType::GnuVersym) {
            return ustl::NONE;
        }
        auto const entries = section_bytes(section.value());
        if (entries.is_none()) {
            return ustl::NONE;
        }
        return VersionSymbolTableView<Traits>::from(entries.value(), m_byte_order);
    }

    [[nodiscard]]
    auto version_definition_table(usize section_index) const noexcept
        -> ustl::Option<VersionDefinitionTableView<Traits>> {
        auto const section = m_sections.at(section_index);
        if (section.is_none() ||
            static_cast<SectionType>(section->sh_type) != SectionType::GnuVerdef) {
            return ustl::NONE;
        }
        auto const string_table_index = to_usize(section->sh_link);
        ustl::Option<Section> string_table;
        if (string_table_index.is_some()) {
            string_table = m_sections.at(string_table_index.value());
        } else {
            string_table = ustl::NONE;
        }
        auto const entries = section_bytes(section.value());
        ustl::Option<ByteRange> strings;
        if (string_table.is_some()) {
            strings = section_bytes(string_table.value());
        } else {
            strings = ustl::NONE;
        }
        if (entries.is_none() || strings.is_none()) {
            return ustl::NONE;
        }
        return VersionDefinitionTableView<Traits>::from(entries.value(), strings.value(),
                                                        m_byte_order);
    }

    [[nodiscard]]
    auto version_requirement_table(usize section_index) const noexcept
        -> ustl::Option<VersionRequirementTableView<Traits>> {
        auto const section = m_sections.at(section_index);
        if (section.is_none() ||
            static_cast<SectionType>(section->sh_type) != SectionType::GnuVerneed) {
            return ustl::NONE;
        }
        auto const string_table_index = to_usize(section->sh_link);
        ustl::Option<Section> string_table;
        if (string_table_index.is_some()) {
            string_table = m_sections.at(string_table_index.value());
        } else {
            string_table = ustl::NONE;
        }
        auto const entries = section_bytes(section.value());
        ustl::Option<ByteRange> strings;
        if (string_table.is_some()) {
            strings = section_bytes(string_table.value());
        } else {
            strings = ustl::NONE;
        }
        if (entries.is_none() || strings.is_none()) {
            return ustl::NONE;
        }
        return VersionRequirementTableView<Traits>::from(entries.value(), strings.value(),
                                                         m_byte_order);
    }

    [[nodiscard]]
    auto version_binding(usize dynamic_symbols_index, usize version_symbols_index,
                         usize symbol_index) const noexcept
        -> ustl::Option<VersionBinding<Traits>> {
        auto const versions = linked_version_symbols(dynamic_symbols_index, version_symbols_index);
        if (versions.is_none()) {
            return ustl::NONE;
        }
        return VersionResolver<Traits>::resolve_base(versions.value(), symbol_index);
    }

    [[nodiscard]]
    auto version_definition_binding(usize dynamic_symbols_index, usize version_symbols_index,
                                    usize definition_index, usize symbol_index) const noexcept
        -> ustl::Option<VersionBinding<Traits>> {
        auto const versions = linked_version_symbols(dynamic_symbols_index, version_symbols_index);
        auto const definitions = version_definition_table(definition_index);
        if (versions.is_none() || definitions.is_none()) {
            return ustl::NONE;
        }
        return VersionResolver<Traits>::resolve_definition(versions.value(), definitions.value(),
                                                           symbol_index);
    }

    [[nodiscard]]
    auto version_requirement_binding(usize dynamic_symbols_index, usize version_symbols_index,
                                     usize requirement_index, usize symbol_index) const noexcept
        -> ustl::Option<VersionBinding<Traits>> {
        auto const versions = linked_version_symbols(dynamic_symbols_index, version_symbols_index);
        auto const requirements = version_requirement_table(requirement_index);
        if (versions.is_none() || requirements.is_none()) {
            return ustl::NONE;
        }
        return VersionResolver<Traits>::resolve_requirement(versions.value(), requirements.value(),
                                                            symbol_index);
    }

    [[nodiscard]]
    auto sysv_hash_table(usize section_index) const noexcept
        -> ustl::Option<SysvHashTableView<Traits>> {
        auto const section = m_sections.at(section_index);
        if (section.is_none() || static_cast<SectionType>(section->sh_type) != SectionType::Hash) {
            return ustl::NONE;
        }
        auto const symbol_table_index = to_usize(section->sh_link);
        ustl::Option<SymbolTableView<Traits>> symbols;
        if (symbol_table_index.is_some()) {
            symbols = symbol_table(symbol_table_index.value());
        } else {
            symbols = ustl::NONE;
        }
        auto const entries = section_bytes(section.value());
        if (symbols.is_none() || entries.is_none()) {
            return ustl::NONE;
        }
        return SysvHashTableView<Traits>::from(entries.value(), symbols.value(), m_byte_order);
    }

    [[nodiscard]]
    auto gnu_hash_table(usize section_index) const noexcept
        -> ustl::Option<GnuHashTableView<Traits>> {
        auto const section = m_sections.at(section_index);
        if (section.is_none() ||
            static_cast<SectionType>(section->sh_type) != SectionType::GnuHash) {
            return ustl::NONE;
        }
        auto const symbol_table_index = to_usize(section->sh_link);
        ustl::Option<Section> symbol_section;
        if (symbol_table_index.is_some()) {
            symbol_section = m_sections.at(symbol_table_index.value());
        } else {
            symbol_section = ustl::NONE;
        }
        ustl::Option<SymbolTableView<Traits>> symbols;
        if (symbol_table_index.is_some() && symbol_section.is_some() &&
            static_cast<SectionType>(symbol_section->sh_type) == SectionType::Dynsym) {
            symbols = symbol_table(symbol_table_index.value());
        } else {
            symbols = ustl::NONE;
        }
        auto const entries = section_bytes(section.value());
        if (symbols.is_none() || entries.is_none()) {
            return ustl::NONE;
        }
        return GnuHashTableView<Traits>::from(entries.value(), symbols.value(), m_byte_order);
    }

    [[nodiscard]]
    auto dynamic_table(usize section_index) const noexcept
        -> ustl::Option<DynamicTableView<Traits>> {
        auto const section = m_sections.at(section_index);
        if (section.is_none() ||
            static_cast<SectionType>(section->sh_type) != SectionType::Dynamic) {
            return ustl::NONE;
        }
        auto const string_table_index = to_usize(section->sh_link);
        ustl::Option<Section> string_table;
        if (string_table_index.is_some()) {
            string_table = m_sections.at(string_table_index.value());
        } else {
            string_table = ustl::NONE;
        }
        auto const entries = section_bytes(section.value());
        ustl::Option<ByteRange> strings;
        if (string_table.is_some()) {
            strings = section_bytes(string_table.value());
        } else {
            strings = ustl::NONE;
        }
        if (entries.is_none() || strings.is_none()) {
            return ustl::NONE;
        }
        return DynamicTableView<Traits>::from(entries.value(), strings.value(), m_byte_order);
    }

    /// Returns the sectionless native-image dynamic table when PT_DYNAMIC has one file mapping.
    [[nodiscard]]
    auto dynamic_image() const noexcept -> ustl::Option<DynamicImageView<Traits>> {
        return DynamicImageView<Traits>::from(m_stream, m_segments, m_byte_order);
    }

    [[nodiscard]]
    auto native_image() const noexcept -> ustl::Option<BasicImage<Traits>>;

    [[nodiscard]]
    auto rel_table(usize section_index) const noexcept
        -> ustl::Option<RelocationTableView<Traits, Rel>> {
        return relocation_table<Rel>(section_index, SectionType::Rel);
    }

    [[nodiscard]]
    auto rela_table(usize section_index) const noexcept
        -> ustl::Option<RelocationTableView<Traits, Rela>> {
        return relocation_table<Rela>(section_index, SectionType::Rela);
    }

    [[nodiscard]]
    auto note_section(usize section_index) const noexcept -> ustl::Option<NoteSectionView<Traits>> {
        auto const section = m_sections.at(section_index);
        if (section.is_none() || static_cast<SectionType>(section->sh_type) != SectionType::Note) {
            return ustl::NONE;
        }
        auto const bytes = section_bytes(section.value());
        if (bytes.is_none()) {
            return ustl::NONE;
        }
        return NoteSectionView<Traits>::from(bytes.value(), m_byte_order);
    }

    [[nodiscard]]
    auto note_segment(usize segment_index) const noexcept -> ustl::Option<NoteSectionView<Traits>> {
        auto const segment = m_segments.at(segment_index);
        if (segment.is_none() || static_cast<SegType>(segment->p_type) != SegType::Note) {
            return ustl::NONE;
        }
        auto const bytes = segment_bytes(segment.value());
        if (bytes.is_none()) {
            return ustl::NONE;
        }
        return NoteSectionView<Traits>::from(bytes.value(), m_byte_order);
    }

    [[nodiscard]]
    auto gnu_property_note(usize section_index, usize note_index) const noexcept
        -> ustl::Option<GnuPropertyNoteView<Traits>> {
        auto const notes = note_section(section_index);
        ustl::Option<NoteRecord> note;
        if (notes.is_some()) {
            note = notes->note_at(note_index);
        } else {
            note = ustl::NONE;
        }
        if (note.is_none()) {
            return ustl::NONE;
        }
        return GnuPropertyNoteView<Traits>::from(note.value(), m_byte_order);
    }

    [[nodiscard]]
    auto gnu_property_segment(usize segment_index, usize note_index) const noexcept
        -> ustl::Option<GnuPropertyNoteView<Traits>> {
        auto const segment = m_segments.at(segment_index);
        if (segment.is_none() || static_cast<SegType>(segment->p_type) != SegType::GnuProperty) {
            return ustl::NONE;
        }
        auto const bytes = segment_bytes(segment.value());
        ustl::Option<NoteSectionView<Traits>> notes;
        if (bytes.is_some()) {
            notes = NoteSectionView<Traits>::from(bytes.value(), m_byte_order);
        } else {
            notes = ustl::NONE;
        }
        ustl::Option<NoteRecord> note;
        if (notes.is_some()) {
            note = notes->note_at(note_index);
        } else {
            note = ustl::NONE;
        }
        if (note.is_none()) {
            return ustl::NONE;
        }
        return GnuPropertyNoteView<Traits>::from(note.value(), m_byte_order);
    }

    /// Returns bytes borrowed from the input ELF image. The returned range remains valid only
    /// while the bytes passed to from_raw() remain valid.
    [[nodiscard]]
    auto file_bytes(usize offset, usize count) const -> ustl::Option<ByteRange> {
        auto const bytes = m_stream.bytes(offset, count);
        if (bytes.is_err()) {
            return ustl::NONE;
        }
        return ustl::some(ByteRange(bytes->data(), bytes->size()));
    }

  private:
    explicit constexpr FileImpl(BinaryStream stream) noexcept : m_stream(stream) {}

    [[nodiscard]]
    auto initialize() -> ustl::Option<ParseError> {
        auto const ident_bytes = m_stream.bytes(0, 7);
        if (ident_bytes.is_err()) {
            return ustl::some(ParseError::InvalidHeader);
        }
        auto const *const ident = ident_bytes.unwrap().data();
        if (std::memcmp(ident, ELF_MAGIC, sizeof(ELF_MAGIC)) != 0 ||
            ident[4] != Traits::ELF_CLASS || ident[6] != 1) {
            return ustl::some(ParseError::InvalidHeader);
        }
        if (ident[5] == 1) {
            m_byte_order = ByteOrder::Little;
        } else if (ident[5] == 2) {
            m_byte_order = ByteOrder::Big;
        } else {
            return ustl::some(ParseError::InvalidHeader);
        }

        auto const header_bytes = m_stream.bytes(0, sizeof(Header));
        if (header_bytes.is_err()) {
            return ustl::some(ParseError::InvalidHeader);
        }
        if (m_byte_order == ByteOrder::Little) {
            m_header = RecordCodec<Traits, Header>::decode_little(header_bytes.unwrap().data());
        } else {
            m_header = RecordCodec<Traits, Header>::decode_big(header_bytes.unwrap().data());
        }

        if (m_header.e_version != 1 || m_header.e_ehsize != sizeof(Header)) {
            return ustl::some(ParseError::InvalidHeader);
        }

        auto const table_counts = resolve_table_counts();
        if (table_counts.is_err()) {
            return ustl::some(static_cast<ParseError>(table_counts.unwrap_err()));
        }

        if (table_counts->m_program_headers != 0) {
            auto const segments = table<Segment>(m_header.e_phoff, table_counts->m_program_headers,
                                                 m_header.e_phentsize, ParseError::InvalidSegment);
            if (segments.is_err()) {
                return ustl::some(static_cast<ParseError>(segments.unwrap_err()));
            }
            m_segments = segments.unwrap();
        }
        if (table_counts->m_sections != 0) {
            auto const sections = table<Section>(m_header.e_shoff, table_counts->m_sections,
                                                 m_header.e_shentsize, ParseError::InvalidSection);
            if (sections.is_err()) {
                return ustl::some(static_cast<ParseError>(sections.unwrap_err()));
            }
            m_sections = sections.unwrap();
        }
        if (!initialize_sections(table_counts->m_section_name_table)) {
            return ustl::some(ParseError::InvalidSection);
        }
        if (!initialize_note_sections()) {
            return ustl::some(ParseError::InvalidNote);
        }
        if (!initialize_note_segments()) {
            return ustl::some(ParseError::InvalidNote);
        }
        if (!initialize_gnu_property_notes()) {
            return ustl::some(ParseError::InvalidNote);
        }
        if (!initialize_gnu_property_segments()) {
            return ustl::some(ParseError::InvalidNote);
        }
        if (!initialize_dynamic_sections()) {
            return ustl::some(ParseError::InvalidDynamic);
        }
        if (!initialize_symbol_sections()) {
            return ustl::some(ParseError::InvalidSymbol);
        }
        if (!initialize_group_sections()) {
            return ustl::some(ParseError::InvalidSection);
        }
        if (!initialize_compressed_sections()) {
            return ustl::some(ParseError::InvalidSection);
        }
        if (!initialize_sysv_hash_sections()) {
            return ustl::some(ParseError::InvalidHash);
        }
        if (!initialize_gnu_hash_sections()) {
            return ustl::some(ParseError::InvalidHash);
        }
        if (!initialize_version_symbol_sections()) {
            return ustl::some(ParseError::InvalidVersion);
        }
        if (!initialize_version_definition_sections()) {
            return ustl::some(ParseError::InvalidVersion);
        }
        if (!initialize_version_requirement_sections()) {
            return ustl::some(ParseError::InvalidVersion);
        }
        if (!initialize_relocation_sections()) {
            return ustl::some(ParseError::InvalidRelocation);
        }

        bool found_load = false;
        bool found_interpreter = false;
        bool found_tls = false;
        bool found_gnu_stack = false;
        for (auto const segment: m_segments) {
            if (!validate_segment_file_range(segment)) {
                return ustl::some(ParseError::InvalidSegment);
            }
            if (segment.type() == SegType::Interp) {
                if (found_load || found_interpreter || interpreter_path_for(segment).is_none()) {
                    return ustl::some(ParseError::InvalidSegment);
                }
                found_interpreter = true;
                continue;
            }
            if (segment.type() == SegType::Tls) {
                if (found_tls || tls_template_for(segment).is_none()) {
                    return ustl::some(ParseError::InvalidSegment);
                }
                found_tls = true;
                continue;
            }
            if (segment.type() == SegType::GnuStack) {
                if (found_gnu_stack || GnuStack<Traits>::from(segment).is_none()) {
                    return ustl::some(ParseError::InvalidSegment);
                }
                found_gnu_stack = true;
                continue;
            }
            if (segment.type() != SegType::Load) {
                continue;
            }
            if (!validate_load_segment(segment)) {
                return ustl::some(ParseError::InvalidSegment);
            }
            found_load = true;
        }

        auto const relro_regions = RelroRegions<Traits>::from(m_segments);
        if (relro_regions.is_none()) {
            return ustl::some(ParseError::InvalidSegment);
        }
        m_relro_regions = relro_regions.value();
        return ustl::NONE;
    }

    template <typename Value>
    [[nodiscard]]
    static constexpr auto to_usize(Value value) noexcept -> ustl::Option<usize> {
        static_assert(std::is_integral_v<Value> && std::is_unsigned_v<Value>);
        if constexpr (sizeof(Value) > sizeof(usize)) {
            if (value > static_cast<Value>(std::numeric_limits<usize>::max())) {
                return ustl::NONE;
            }
        }
        return ustl::some(static_cast<usize>(value));
    }

    struct HeaderTableCounts final {
        usize m_program_headers{0};
        usize m_sections{0};
        ustl::Option<usize> m_section_name_table;
    };

    [[nodiscard]]
    auto resolve_table_counts() -> ustl::Result<HeaderTableCounts, ParseError> {
        if ((m_header.e_shnum != 0 && m_header.e_shnum >= SHN_LORESERVE) ||
            (m_header.e_shstrndx >= SHN_LORESERVE && m_header.e_shstrndx != SHN_XINDEX)) {
            return ustl::err(ParseError::InvalidSection);
        }

        auto const needs_section_zero = m_header.e_phnum == PN_XNUM ||
                                        (m_header.e_shnum == 0 && m_header.e_shoff != 0) ||
                                        m_header.e_shstrndx == SHN_XINDEX;

        Section section_zero{};
        if (needs_section_zero) {
            auto const table_zero = table<Section>(m_header.e_shoff, usize{1}, m_header.e_shentsize,
                                                   ParseError::InvalidSection);
            if (table_zero.is_err()) {
                return ustl::err(table_zero.unwrap_err());
            }
            section_zero = table_zero.unwrap()[0];
            if (static_cast<SectionType>(section_zero.sh_type) != SectionType::Null) {
                return ustl::err(ParseError::InvalidSection);
            }
        }

        HeaderTableCounts counts{};
        if (m_header.e_phnum == PN_XNUM) {
            auto const program_headers = to_usize(section_zero.sh_info);
            if (program_headers.is_none() ||
                program_headers.value() < static_cast<usize>(PN_XNUM)) {
                return ustl::err(ParseError::InvalidSegment);
            }
            counts.m_program_headers = program_headers.value();
        } else {
            counts.m_program_headers = static_cast<usize>(m_header.e_phnum);
        }

        if (m_header.e_shnum == 0 && m_header.e_shoff != 0) {
            auto const sections = to_usize(section_zero.sh_size);
            if (sections.is_none() || sections.value() < static_cast<usize>(SHN_LORESERVE)) {
                return ustl::err(ParseError::InvalidSection);
            }
            counts.m_sections = sections.value();
        } else {
            counts.m_sections = static_cast<usize>(m_header.e_shnum);
        }

        if (m_header.e_shstrndx == SHN_XINDEX) {
            auto const name_table_index = to_usize(section_zero.sh_link);
            if (name_table_index.is_none() ||
                name_table_index.value() < static_cast<usize>(SHN_LORESERVE)) {
                return ustl::err(ParseError::InvalidSection);
            }
            counts.m_section_name_table = ustl::Option<usize>(name_table_index.value());
        } else if (m_header.e_shstrndx != 0) {
            counts.m_section_name_table =
                ustl::Option<usize>(static_cast<usize>(m_header.e_shstrndx));
        }
        return ustl::ok(std::move(counts));
    }

    template <typename Entry, typename Offset, typename Count, typename EntrySize>
    [[nodiscard]]
    auto table(Offset offset, Count count, EntrySize entry_size, ParseError error)
        -> ustl::Result<TableView<Entry>, ParseError> {
        if (static_cast<usize>(entry_size) != sizeof(Entry)) {
            return ustl::err(error);
        }

        auto const table_offset = to_usize(offset);
        auto const entry_count = to_usize(count);
        if (table_offset.is_none() || entry_count.is_none() ||
            entry_count.value() > std::numeric_limits<usize>::max() / sizeof(Entry)) {
            return ustl::err(error);
        }

        auto const table_bytes =
            m_stream.bytes(table_offset.value(), entry_count.value() * sizeof(Entry));
        if (table_bytes.is_err()) {
            return ustl::err(error);
        }
        typename TableView<Entry>::Decoder decoder;
        if (m_byte_order == ByteOrder::Little) {
            decoder = &RecordCodec<Traits, Entry>::decode_little;
        } else {
            decoder = &RecordCodec<Traits, Entry>::decode_big;
        }
        auto const table_view = TableView<Entry>::from_checked_bytes(table_bytes.unwrap(),
                                                                     entry_count.value(), decoder);
        if (table_view.is_none()) {
            return ustl::err(error);
        }
        return ustl::ok(table_view.value());
    }

    [[nodiscard]]
    auto validate_segment_file_range(Segment const &segment) const noexcept -> bool {
        auto const file_size = to_usize(segment.p_filesz);
        if (file_size.is_none()) {
            return false;
        }
        if (file_size.value() == 0) {
            return true;
        }

        auto const file_offset = to_usize(segment.p_offset);
        return file_offset.is_some() &&
               m_stream.bytes(file_offset.value(), file_size.value()).is_ok();
    }

    [[nodiscard]]
    auto interpreter_path_for(Segment const &segment) const noexcept -> ustl::Option<ByteRange> {
        auto const file_offset = to_usize(segment.p_offset);
        auto const file_size = to_usize(segment.p_filesz);
        if (file_offset.is_none() || file_size.is_none() || file_size.value() == 0) {
            return ustl::NONE;
        }
        auto const bytes = file_bytes(file_offset.value(), file_size.value());
        if (bytes.is_none() || bytes->data()[bytes->size() - 1] != 0) {
            return ustl::NONE;
        }
        for (usize index = 0; index + 1 < bytes->size(); ++index) {
            if (bytes->data()[index] == 0) {
                return ustl::NONE;
            }
        }
        return ustl::some(ByteRange(bytes->data(), bytes->size() - 1));
    }

    [[nodiscard]]
    auto tls_template_for(Segment const &segment) const noexcept
        -> ustl::Option<TlsTemplate<Traits>> {
        auto const file_size = to_usize(segment.p_filesz);
        if (file_size.is_none()) {
            return ustl::NONE;
        }
        if (file_size.value() == 0) {
            return TlsTemplate<Traits>::from(segment, ByteRange{});
        }

        auto const file_offset = to_usize(segment.p_offset);
        ustl::Option<ByteRange> initialized;
        if (file_offset.is_some()) {
            initialized = file_bytes(file_offset.value(), file_size.value());
        } else {
            initialized = ustl::NONE;
        }
        if (initialized.is_none()) {
            return ustl::NONE;
        }
        return TlsTemplate<Traits>::from(segment, initialized.value());
    }

    [[nodiscard]]
    auto section_bytes(Section const &section) const noexcept -> ustl::Option<ByteRange> {
        auto const section_type = static_cast<SectionType>(section.sh_type);
        if (section_type == SectionType::Null || section_type == SectionType::Nobits) {
            return ustl::NONE;
        }
        auto const file_offset = to_usize(section.sh_offset);
        auto const file_size = to_usize(section.sh_size);
        if (file_offset.is_none() || file_size.is_none()) {
            return ustl::NONE;
        }
        return file_bytes(file_offset.value(), file_size.value());
    }

    [[nodiscard]]
    auto segment_bytes(Segment const &segment) const noexcept -> ustl::Option<ByteRange> {
        auto const file_offset = to_usize(segment.p_offset);
        auto const file_size = to_usize(segment.p_filesz);
        if (file_offset.is_some() && file_size.is_some()) {
            return file_bytes(file_offset.value(), file_size.value());
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    auto initialize_sections(ustl::Option<usize> const &name_table_index) -> bool {
        if (!m_sections.empty() &&
            static_cast<SectionType>(m_sections[0].sh_type) != SectionType::Null) {
            return false;
        }
        for (auto const section: m_sections) {
            auto const section_type = static_cast<SectionType>(section.sh_type);
            auto const alignment = static_cast<u64>(section.sh_addralign);
            if ((alignment > 1 && (alignment & (alignment - 1)) != 0) ||
                (section.sh_entsize != 0 && section.sh_size % section.sh_entsize != 0) ||
                (section_type != SectionType::Null && section_type != SectionType::Nobits &&
                 section_bytes(section).is_none())) {
                return false;
            }
        }

        if (name_table_index.is_none()) {
            return true;
        }
        auto const name_table = m_sections.at(name_table_index.value());
        if (name_table.is_none() ||
            static_cast<SectionType>(name_table->sh_type) != SectionType::Strtab) {
            return false;
        }
        auto const bytes = section_bytes(name_table.value());
        if (bytes.is_none()) {
            return false;
        }
        auto const names = StringTableView::from(bytes.value());
        if (names.is_none()) {
            return false;
        }
        m_section_names = names.value();
        return true;
    }

    [[nodiscard]]
    auto initialize_symbol_sections() const noexcept -> bool {
        for (auto const section: m_sections) {
            auto const section_type = static_cast<SectionType>(section.sh_type);
            if (section_type != SectionType::Symtab && section_type != SectionType::Dynsym) {
                continue;
            }
            auto const entry_count = to_usize(section.sh_size);
            auto const linked_section = m_sections.at(static_cast<usize>(section.sh_link));
            if (section.sh_entsize != sizeof(Symbol) || entry_count.is_none() ||
                entry_count.value() % sizeof(Symbol) != 0 ||
                section.sh_info > entry_count.value() / sizeof(Symbol) ||
                linked_section.is_none() ||
                static_cast<SectionType>(linked_section->sh_type) != SectionType::Strtab) {
                return false;
            }
        }

        if (!initialize_symbol_section_indices()) {
            return false;
        }

        for (usize section_index = 0; section_index < m_sections.size(); ++section_index) {
            auto const section = m_sections.at(section_index);
            if (section.is_none() ||
                (static_cast<SectionType>(section->sh_type) != SectionType::Symtab &&
                 static_cast<SectionType>(section->sh_type) != SectionType::Dynsym)) {
                continue;
            }
            auto const entries = section_bytes(section.value());
            if (entries.is_none()) {
                return false;
            }
            typename TableView<Symbol>::Decoder symbol_decoder;
            if (m_byte_order == ByteOrder::Little) {
                symbol_decoder = &RecordCodec<Traits, Symbol>::decode_little;
            } else {
                symbol_decoder = &RecordCodec<Traits, Symbol>::decode_big;
            }
            auto const symbols = TableView<Symbol>::from_checked_bytes(
                entries.value(), entries->size() / sizeof(Symbol), symbol_decoder);
            if (symbols.is_none()) {
                return false;
            }
            for (auto const symbol: symbols.value()) {
                if (symbol.st_shndx == static_cast<typename Traits::Half>(SHN_XINDEX) &&
                    symbol_section_indices(section_index).is_none()) {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]]
    auto initialize_group_sections() const noexcept -> bool {
        constexpr auto GROUP_FLAG = static_cast<u64>(SectionFlags::Group);
        constexpr typename Traits::Half ET_REL = 1;

        for (usize group_index = 0; group_index < m_sections.size(); ++group_index) {
            auto const section = m_sections.at(group_index);
            if (section.is_none() ||
                static_cast<SectionType>(section->sh_type) != SectionType::Group) {
                continue;
            }
            if (m_header.e_type != ET_REL) {
                return false;
            }
            auto const group = section_group(group_index);
            if (group.is_none()) {
                return false;
            }
            for (usize member_index = 0; member_index < group->member_count(); ++member_index) {
                auto const member_section_index = group->member_section_at(member_index);
                ustl::Option<Section> member_section;
                if (member_section_index.is_some()) {
                    member_section = m_sections.at(member_section_index.value());
                } else {
                    member_section = ustl::NONE;
                }
                if (member_section_index.is_none() || member_section_index.value() <= group_index ||
                    member_section.is_none() ||
                    (static_cast<u64>(member_section->sh_flags) & GROUP_FLAG) == 0) {
                    return false;
                }
                for (usize earlier_group_index = 0; earlier_group_index < group_index;
                     ++earlier_group_index) {
                    auto const earlier_section = m_sections.at(earlier_group_index);
                    if (earlier_section.is_none() ||
                        static_cast<SectionType>(earlier_section->sh_type) != SectionType::Group) {
                        continue;
                    }
                    auto const earlier_group = section_group(earlier_group_index);
                    if (earlier_group.is_none()) {
                        return false;
                    }
                    for (usize earlier_member_index = 0;
                         earlier_member_index < earlier_group->member_count();
                         ++earlier_member_index) {
                        auto const earlier_member =
                            earlier_group->member_section_at(earlier_member_index);
                        if (earlier_member.is_none() ||
                            earlier_member.value() == member_section_index.value()) {
                            return false;
                        }
                    }
                }
            }
        }

        for (usize section_index = 0; section_index < m_sections.size(); ++section_index) {
            auto const section = m_sections.at(section_index);
            if (section.is_none() || (static_cast<u64>(section->sh_flags) & GROUP_FLAG) == 0) {
                continue;
            }
            if (m_header.e_type != ET_REL) {
                return false;
            }
            auto is_member = false;
            for (usize group_index = 0; group_index < section_index; ++group_index) {
                auto const group_section = m_sections.at(group_index);
                if (group_section.is_none() ||
                    static_cast<SectionType>(group_section->sh_type) != SectionType::Group) {
                    continue;
                }
                auto const group = section_group(group_index);
                if (group.is_none()) {
                    return false;
                }
                for (usize member_index = 0; member_index < group->member_count(); ++member_index) {
                    auto const member_section_index = group->member_section_at(member_index);
                    if (member_section_index.is_none()) {
                        return false;
                    }
                    is_member = is_member || member_section_index.value() == section_index;
                }
            }
            if (!is_member) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    auto initialize_compressed_sections() const noexcept -> bool {
        constexpr auto ALLOC_FLAG = static_cast<u64>(SectionFlags::Alloc);
        constexpr auto COMPRESSED_FLAG = static_cast<u64>(SectionFlags::Compressed);

        for (usize section_index = 0; section_index < m_sections.size(); ++section_index) {
            auto const section = m_sections.at(section_index);
            if (section.is_none() || (static_cast<u64>(section->sh_flags) & COMPRESSED_FLAG) == 0) {
                continue;
            }
            if ((static_cast<u64>(section->sh_flags) & ALLOC_FLAG) != 0 ||
                compressed_section(section_index).is_none()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    auto initialize_symbol_section_indices() const noexcept -> bool {
        for (usize section_index = 0; section_index < m_sections.size(); ++section_index) {
            auto const section = m_sections.at(section_index);
            if (section.is_none() ||
                static_cast<SectionType>(section->sh_type) != SectionType::SymtabShndx) {
                continue;
            }

            auto const symbol_section_index = to_usize(section->sh_link);
            auto const index_bytes = to_usize(section->sh_size);
            ustl::Option<Section> symbol_section;
            if (symbol_section_index.is_some()) {
                symbol_section = m_sections.at(symbol_section_index.value());
            } else {
                symbol_section = ustl::NONE;
            }
            if (section->sh_entsize != sizeof(u32) || section->sh_info != 0 ||
                index_bytes.is_none() || symbol_section.is_none() ||
                (static_cast<SectionType>(symbol_section->sh_type) != SectionType::Symtab &&
                 static_cast<SectionType>(symbol_section->sh_type) != SectionType::Dynsym)) {
                return false;
            }

            auto const symbol_bytes = to_usize(symbol_section->sh_size);
            if (symbol_bytes.is_none() || symbol_bytes.value() % sizeof(Symbol) != 0) {
                return false;
            }
            auto const symbol_count = symbol_bytes.value() / sizeof(Symbol);
            if (symbol_count > std::numeric_limits<usize>::max() / sizeof(u32) ||
                index_bytes.value() != symbol_count * sizeof(u32)) {
                return false;
            }

            for (usize earlier_index = 0; earlier_index < section_index; ++earlier_index) {
                auto const earlier = m_sections.at(earlier_index);
                if (earlier.is_some() &&
                    static_cast<SectionType>(earlier->sh_type) == SectionType::SymtabShndx &&
                    to_usize(earlier->sh_link).is_some() &&
                    to_usize(earlier->sh_link).value() == symbol_section_index.value()) {
                    return false;
                }
            }

            auto const index_data = section_bytes(section.value());
            auto const symbol_data = section_bytes(symbol_section.value());
            if (index_data.is_none() || symbol_data.is_none()) {
                return false;
            }
            TableView<u32>::Decoder index_decoder;
            if (m_byte_order == ByteOrder::Little) {
                index_decoder = &ScalarCodec<u32>::decode_little;
            } else {
                index_decoder = &ScalarCodec<u32>::decode_big;
            }
            typename TableView<Symbol>::Decoder symbol_decoder;
            if (m_byte_order == ByteOrder::Little) {
                symbol_decoder = &RecordCodec<Traits, Symbol>::decode_little;
            } else {
                symbol_decoder = &RecordCodec<Traits, Symbol>::decode_big;
            }
            auto const indices =
                TableView<u32>::from_checked_bytes(index_data.value(), symbol_count, index_decoder);
            auto const symbols = TableView<Symbol>::from_checked_bytes(
                symbol_data.value(), symbol_count, symbol_decoder);
            if (indices.is_none() || symbols.is_none()) {
                return false;
            }
            for (usize symbol_index = 0; symbol_index < symbol_count; ++symbol_index) {
                auto const symbol = symbols->at(symbol_index);
                auto const index = indices->at(symbol_index);
                if (symbol.is_none() || index.is_none() ||
                    (symbol->st_shndx != static_cast<typename Traits::Half>(SHN_XINDEX) &&
                     index.value() != 0) ||
                    (symbol->st_shndx == static_cast<typename Traits::Half>(SHN_XINDEX) &&
                     static_cast<usize>(index.value()) >= m_sections.size())) {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]]
    auto symbol_section_indices(usize symbol_section_index) const noexcept
        -> ustl::Option<ByteRange> {
        for (auto const section: m_sections) {
            if (static_cast<SectionType>(section.sh_type) != SectionType::SymtabShndx) {
                continue;
            }
            auto const linked_index = to_usize(section.sh_link);
            if (linked_index.is_some() && linked_index.value() == symbol_section_index) {
                return section_bytes(section);
            }
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    auto linked_version_symbols(usize dynamic_symbols_index,
                                usize version_symbols_index) const noexcept
        -> ustl::Option<VersionSymbolTableView<Traits>> {
        auto const dynamic_symbols = m_sections.at(dynamic_symbols_index);
        auto const version_symbols = m_sections.at(version_symbols_index);
        if (dynamic_symbols.is_none() || version_symbols.is_none() ||
            static_cast<SectionType>(dynamic_symbols->sh_type) != SectionType::Dynsym ||
            static_cast<SectionType>(version_symbols->sh_type) != SectionType::GnuVersym ||
            to_usize(version_symbols->sh_link).is_none() ||
            to_usize(version_symbols->sh_link).value() != dynamic_symbols_index) {
            return ustl::NONE;
        }
        return version_symbol_table(version_symbols_index);
    }

    [[nodiscard]]
    auto initialize_version_symbol_sections() const noexcept -> bool {
        for (usize section_index = 0; section_index < m_sections.size(); ++section_index) {
            auto const section = m_sections.at(section_index);
            if (section.is_none() ||
                static_cast<SectionType>(section->sh_type) != SectionType::GnuVersym) {
                continue;
            }

            auto const symbol_section_index = to_usize(section->sh_link);
            ustl::Option<Section> symbol_section;
            if (symbol_section_index.is_some()) {
                symbol_section = m_sections.at(symbol_section_index.value());
            } else {
                symbol_section = ustl::NONE;
            }
            auto const version_bytes = section_bytes(section.value());
            if (section->sh_entsize != sizeof(typename Traits::Half) || symbol_section.is_none() ||
                static_cast<SectionType>(symbol_section->sh_type) != SectionType::Dynsym ||
                version_bytes.is_none()) {
                return false;
            }

            auto const symbol_bytes = to_usize(symbol_section->sh_size);
            if (symbol_bytes.is_none() || symbol_bytes.value() % sizeof(Symbol) != 0) {
                return false;
            }
            auto const versions =
                VersionSymbolTableView<Traits>::from(version_bytes.value(), m_byte_order);
            if (versions.is_none() || versions->size() != symbol_bytes.value() / sizeof(Symbol)) {
                return false;
            }

            for (usize earlier_index = 0; earlier_index < section_index; ++earlier_index) {
                auto const earlier = m_sections.at(earlier_index);
                if (earlier.is_some() &&
                    static_cast<SectionType>(earlier->sh_type) == SectionType::GnuVersym &&
                    to_usize(earlier->sh_link).is_some() &&
                    to_usize(earlier->sh_link).value() == symbol_section_index.value()) {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]]
    auto initialize_sysv_hash_sections() const noexcept -> bool {
        for (auto const section: m_sections) {
            if (static_cast<SectionType>(section.sh_type) != SectionType::Hash) {
                continue;
            }
            auto const symbol_table_index = to_usize(section.sh_link);
            ustl::Option<SymbolTableView<Traits>> symbols;
            if (symbol_table_index.is_some()) {
                symbols = symbol_table(symbol_table_index.value());
            } else {
                symbols = ustl::NONE;
            }
            auto const entries = section_bytes(section);
            if (symbols.is_none() || entries.is_none() ||
                SysvHashTableView<Traits>::from(entries.value(), symbols.value(), m_byte_order)
                    .is_none()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    auto initialize_gnu_hash_sections() const noexcept -> bool {
        for (auto const section: m_sections) {
            if (static_cast<SectionType>(section.sh_type) != SectionType::GnuHash) {
                continue;
            }
            auto const symbol_table_index = to_usize(section.sh_link);
            ustl::Option<Section> symbol_section;
            if (symbol_table_index.is_some()) {
                symbol_section = m_sections.at(symbol_table_index.value());
            } else {
                symbol_section = ustl::NONE;
            }
            ustl::Option<SymbolTableView<Traits>> symbols;
            if (symbol_table_index.is_some() && symbol_section.is_some() &&
                static_cast<SectionType>(symbol_section->sh_type) == SectionType::Dynsym) {
                symbols = symbol_table(symbol_table_index.value());
            } else {
                symbols = ustl::NONE;
            }
            auto const entries = section_bytes(section);
            if (symbols.is_none() || entries.is_none() ||
                GnuHashTableView<Traits>::from(entries.value(), symbols.value(), m_byte_order)
                    .is_none()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    auto initialize_version_definition_sections() const noexcept -> bool {
        for (auto const section: m_sections) {
            if (static_cast<SectionType>(section.sh_type) != SectionType::GnuVerdef) {
                continue;
            }
            auto const string_table_index = to_usize(section.sh_link);
            ustl::Option<Section> string_table;
            if (string_table_index.is_some()) {
                string_table = m_sections.at(string_table_index.value());
            } else {
                string_table = ustl::NONE;
            }
            auto const entries = section_bytes(section);
            ustl::Option<ByteRange> strings;
            if (string_table.is_some()) {
                strings = section_bytes(string_table.value());
            } else {
                strings = ustl::NONE;
            }
            if (section.sh_entsize != 0 || string_table.is_none() ||
                static_cast<SectionType>(string_table->sh_type) != SectionType::Strtab ||
                entries.is_none() || strings.is_none() ||
                VersionDefinitionTableView<Traits>::from(entries.value(), strings.value(),
                                                         m_byte_order)
                    .is_none()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    auto initialize_version_requirement_sections() const noexcept -> bool {
        for (auto const section: m_sections) {
            if (static_cast<SectionType>(section.sh_type) != SectionType::GnuVerneed) {
                continue;
            }
            auto const string_table_index = to_usize(section.sh_link);
            ustl::Option<Section> string_table;
            if (string_table_index.is_some()) {
                string_table = m_sections.at(string_table_index.value());
            } else {
                string_table = ustl::NONE;
            }
            auto const entries = section_bytes(section);
            ustl::Option<ByteRange> strings;
            if (string_table.is_some()) {
                strings = section_bytes(string_table.value());
            } else {
                strings = ustl::NONE;
            }
            if (section.sh_entsize != 0 || string_table.is_none() ||
                static_cast<SectionType>(string_table->sh_type) != SectionType::Strtab ||
                entries.is_none() || strings.is_none() ||
                VersionRequirementTableView<Traits>::from(entries.value(), strings.value(),
                                                          m_byte_order)
                    .is_none()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    auto initialize_note_sections() const noexcept -> bool {
        for (auto const section: m_sections) {
            if (static_cast<SectionType>(section.sh_type) != SectionType::Note) {
                continue;
            }
            auto const bytes = section_bytes(section);
            if (bytes.is_none() ||
                NoteSectionView<Traits>::from(bytes.value(), m_byte_order).is_none()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    auto initialize_note_segments() const noexcept -> bool {
        for (usize segment_index = 0; segment_index < m_segments.size(); ++segment_index) {
            auto const segment = m_segments.at(segment_index);
            if (segment.is_none() || static_cast<SegType>(segment->p_type) != SegType::Note) {
                continue;
            }
            if (note_segment(segment_index).is_none()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    auto initialize_gnu_property_notes() const noexcept -> bool {
        for (usize section_index = 0; section_index < m_sections.size(); ++section_index) {
            auto const section = m_sections.at(section_index);
            if (section.is_none() ||
                static_cast<SectionType>(section->sh_type) != SectionType::Note) {
                continue;
            }
            auto const notes = note_section(section_index);
            if (notes.is_none()) {
                return false;
            }
            for (usize note_index = 0; note_index < notes->size(); ++note_index) {
                auto const note = notes->note_at(note_index);
                if (note.is_none()) {
                    return false;
                }
                if (GnuPropertyNoteView<Traits>::is_gnu_property_note(note.value()) &&
                    GnuPropertyNoteView<Traits>::from(note.value(), m_byte_order).is_none()) {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]]
    auto initialize_gnu_property_segments() const noexcept -> bool {
        auto seen_property_segment = false;
        for (usize segment_index = 0; segment_index < m_segments.size(); ++segment_index) {
            auto const segment = m_segments.at(segment_index);
            if (segment.is_none() ||
                static_cast<SegType>(segment->p_type) != SegType::GnuProperty) {
                continue;
            }
            if (seen_property_segment) {
                return false;
            }
            seen_property_segment = true;
            auto const bytes = segment_bytes(segment.value());
            ustl::Option<NoteSectionView<Traits>> notes;
            if (bytes.is_some()) {
                notes = NoteSectionView<Traits>::from(bytes.value(), m_byte_order);
            } else {
                notes = ustl::NONE;
            }
            ustl::Option<NoteRecord> note;
            if (notes.is_some() && notes->size() == 1) {
                note = notes->note_at(0);
            } else {
                note = ustl::NONE;
            }
            if (note.is_none() ||
                !GnuPropertyNoteView<Traits>::is_gnu_property_note(note.value()) ||
                GnuPropertyNoteView<Traits>::from(note.value(), m_byte_order).is_none()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    auto initialize_dynamic_sections() const noexcept -> bool {
        for (auto const section: m_sections) {
            if (static_cast<SectionType>(section.sh_type) != SectionType::Dynamic) {
                continue;
            }
            auto const string_table_index = to_usize(section.sh_link);
            ustl::Option<Section> string_table;
            if (string_table_index.is_some()) {
                string_table = m_sections.at(string_table_index.value());
            } else {
                string_table = ustl::NONE;
            }
            auto const entries = section_bytes(section);
            ustl::Option<ByteRange> strings;
            if (string_table.is_some()) {
                strings = section_bytes(string_table.value());
            } else {
                strings = ustl::NONE;
            }
            if (section.sh_entsize != sizeof(Dyn) || section.sh_info != 0 ||
                string_table.is_none() ||
                static_cast<SectionType>(string_table->sh_type) != SectionType::Strtab ||
                entries.is_none() || strings.is_none()) {
                return false;
            }

            auto const dynamics =
                DynamicTableView<Traits>::from(entries.value(), strings.value(), m_byte_order);
            if (dynamics.is_none()) {
                return false;
            }
            for (usize index = 0; index < dynamics->size(); ++index) {
                auto const entry = dynamics->entry_at(index);
                if (entry.is_none()) {
                    return false;
                }
                if (is_dynamic_string_tag(DynamicTableView<Traits>::tag(entry.value())) &&
                    dynamics->string_at(entry.value()).is_none()) {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]]
    static constexpr auto is_dynamic_string_tag(DynTag tag) noexcept -> bool {
        return tag == DynTag::Needed || tag == DynTag::SoName || tag == DynTag::Rpath ||
               tag == DynTag::RunPath;
    }

    template <typename Entry>
    [[nodiscard]]
    auto relocation_table(usize section_index, SectionType expected_type) const noexcept
        -> ustl::Option<RelocationTableView<Traits, Entry>> {
        auto const section = m_sections.at(section_index);
        if (section.is_none() || static_cast<SectionType>(section->sh_type) != expected_type) {
            return ustl::NONE;
        }
        auto const entries = section_bytes(section.value());
        if (entries.is_none()) {
            return ustl::NONE;
        }
        return RelocationTableView<Traits, Entry>::from(entries.value(), m_byte_order);
    }

    template <typename Entry>
    [[nodiscard]]
    auto initialize_relocation_section(Section const &section) const noexcept -> bool {
        auto const relocation_bytes = to_usize(section.sh_size);
        auto const symbol_section_index = to_usize(section.sh_link);
        if (section.sh_entsize != sizeof(Entry) || relocation_bytes.is_none() ||
            relocation_bytes.value() % sizeof(Entry) != 0 || symbol_section_index.is_none()) {
            return false;
        }

        auto const symbol_section = m_sections.at(symbol_section_index.value());
        if (symbol_section.is_none() ||
            (static_cast<SectionType>(symbol_section->sh_type) != SectionType::Symtab &&
             static_cast<SectionType>(symbol_section->sh_type) != SectionType::Dynsym)) {
            return false;
        }

        auto const symbol_bytes = to_usize(symbol_section->sh_size);
        auto const entries = section_bytes(section);
        if (symbol_bytes.is_none() || symbol_bytes.value() % sizeof(Symbol) != 0 ||
            entries.is_none()) {
            return false;
        }
        typename TableView<Entry>::Decoder decoder;
        if (m_byte_order == ByteOrder::Little) {
            decoder = &RecordCodec<Traits, Entry>::decode_little;
        } else {
            decoder = &RecordCodec<Traits, Entry>::decode_big;
        }
        auto const relocations = TableView<Entry>::from_checked_bytes(
            entries.value(), relocation_bytes.value() / sizeof(Entry), decoder);
        if (relocations.is_none()) {
            return false;
        }

        auto const symbol_count = symbol_bytes.value() / sizeof(Symbol);
        for (auto const relocation: relocations.value()) {
            if (static_cast<usize>(Traits::relocation_symbol(relocation.r_info)) >= symbol_count) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    auto initialize_relocation_sections() const noexcept -> bool {
        for (auto const section: m_sections) {
            switch (static_cast<SectionType>(section.sh_type)) {
                case SectionType::Rel:
                    if (!initialize_relocation_section<Rel>(section)) {
                        return false;
                    }
                    break;
                case SectionType::Rela:
                    if (!initialize_relocation_section<Rela>(section)) {
                        return false;
                    }
                    break;
                default:
                    break;
            }
        }
        return true;
    }

    [[nodiscard]]
    static constexpr auto validate_load_segment(Segment const &segment) noexcept -> bool {
        auto const file_size = static_cast<u64>(segment.p_filesz);
        auto const memory_size = static_cast<u64>(segment.p_memsz);
        auto const virtual_begin = static_cast<u64>(segment.p_vaddr);
        auto const alignment = static_cast<u64>(segment.p_align);

        if (file_size > memory_size ||
            virtual_begin > std::numeric_limits<u64>::max() - memory_size) {
            return false;
        }
        if (alignment != 0 && (alignment & (alignment - 1)) != 0) {
            return false;
        }
        return alignment <= 1 ||
               static_cast<u64>(segment.p_offset) % alignment == virtual_begin % alignment;
    }

    BinaryStream m_stream{};
    ByteOrder m_byte_order{ByteOrder::Little};
    Header m_header{};
    Segments m_segments{};
    Sections m_sections{};
    RelroRegions<Traits> m_relro_regions{};
    ustl::Option<StringTableView> m_section_names;
};

template <typename Traits>
class BasicImage final: public eftl::Binary {
  public:
    using File = FileImpl<Traits>;
    using Segment = typename Traits::Segment;

    [[nodiscard]]
    static auto from(File const &file) noexcept -> ustl::Option<BasicImage> {
        auto const &header = file.elf_header();
        if (header.e_type != EXECUTABLE && header.e_type != SHARED_OBJECT) {
            return ustl::NONE;
        }

        bool has_load = false;
        bool executable_entry = false;
        auto image_base = std::numeric_limits<u64>::max();
        for (usize index = 0; index < file.segments().size(); ++index) {
            auto const segment = file.segments()[index];
            if (segment.type() != SegType::Load) {
                continue;
            }
            auto const memory_size = static_cast<u64>(segment.p_memsz);
            auto const file_size = static_cast<u64>(segment.p_filesz);
            auto const virtual_begin = static_cast<u64>(segment.p_vaddr);
            if (memory_size == 0 || file_size > memory_size ||
                virtual_begin > std::numeric_limits<u64>::max() - memory_size) {
                return ustl::NONE;
            }
            auto const virtual_end = virtual_begin + memory_size;
            for (usize earlier = 0; earlier < index; ++earlier) {
                auto const other = file.segments()[earlier];
                if (other.type() != SegType::Load || other.p_memsz == 0) {
                    continue;
                }
                auto const other_begin = static_cast<u64>(other.p_vaddr);
                auto const other_size = static_cast<u64>(other.p_memsz);
                if (other_begin > std::numeric_limits<u64>::max() - other_size) {
                    return ustl::NONE;
                }
                auto const other_end = other_begin + other_size;
                if (virtual_begin < other_end && other_begin < virtual_end) {
                    return ustl::NONE;
                }
            }

            has_load = true;
            if (virtual_begin < image_base) {
                image_base = virtual_begin;
            }
            if (header.e_entry >= virtual_begin && header.e_entry < virtual_end &&
                (segment.permissions() & SegPerms::Executable) == SegPerms::Executable) {
                executable_entry = true;
            }
        }
        if (has_load && executable_entry) {
            return ustl::some(BasicImage(file, image_base));
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    auto elf_file() const noexcept -> File const & {
        return m_file;
    }

    [[nodiscard]]
    auto format() const noexcept -> FormatId override {
        return FormatId::Elf;
    }

    [[nodiscard]]
    auto entrypoint() const noexcept -> u64 override {
        return static_cast<u64>(m_file.elf_header().e_entry);
    }

    [[nodiscard]]
    auto imagebase() const noexcept -> u64 override {
        return m_image_base;
    }

    [[nodiscard]]
    auto virtual_address_to_offset(u64 virtual_address) const noexcept
        -> ustl::Option<usize> override {
        for (auto const segment: m_file.segments()) {
            if (segment.type() != SegType::Load) {
                continue;
            }
            auto const virtual_begin = static_cast<u64>(segment.p_vaddr);
            auto const file_size = static_cast<u64>(segment.p_filesz);
            if (virtual_address < virtual_begin || file_size == 0) {
                continue;
            }
            auto const virtual_delta = virtual_address - virtual_begin;
            if (virtual_delta >= file_size) {
                continue;
            }
            auto const file_offset = to_usize(segment.p_offset);
            auto const offset_delta = to_usize(virtual_delta);
            if (file_offset.is_none() || offset_delta.is_none() ||
                file_offset.value() > std::numeric_limits<usize>::max() - offset_delta.value()) {
                return ustl::NONE;
            }
            return ustl::some(file_offset.value() + offset_delta.value());
        }
        return ustl::NONE;
    }

  private:
    static constexpr u16 EXECUTABLE = 2;
    static constexpr u16 SHARED_OBJECT = 3;

    explicit constexpr BasicImage(File const &file, u64 image_base) noexcept
        : m_file(file), m_image_base(image_base) {}

    template <typename Value>
    [[nodiscard]]
    static constexpr auto to_usize(Value value) noexcept -> ustl::Option<usize> {
        static_assert(std::is_integral_v<Value> && std::is_unsigned_v<Value>);
        if constexpr (sizeof(Value) > sizeof(usize)) {
            if (value > static_cast<Value>(std::numeric_limits<usize>::max())) {
                return ustl::NONE;
            }
        }
        return ustl::some(static_cast<usize>(value));
    }

    File const &m_file;
    u64 m_image_base{0};
};

template <typename Traits>
[[nodiscard]]
auto FileImpl<Traits>::native_image() const noexcept -> ustl::Option<BasicImage<Traits>> {
    return BasicImage<Traits>::from(*this);
}

/// A type-erased, ready-to-use ELF image.  Parser::parse() auto-detects 32/64,
/// validates loadability, and returns a Binary that directly implements eftl::Binary.
class Binary final: public eftl::Binary {
  public:
    using FileVariant = ustl::Variant<FileImpl<Traits32>, FileImpl<Traits64>>;
    using ImageVariant = ustl::Variant<BasicImage<Traits32>, BasicImage<Traits64>>;

    /// Constructs a Binary from a parsed FileImpl.  Creates the BasicImage
    /// internally so the FileImpl outlives the BasicImage that references it.
    explicit constexpr Binary(FileVariant file) noexcept
        : m_file(std::move(file)), m_image(make_image(m_file)) {}

  private:
    [[nodiscard]]
    static auto make_image(FileVariant const &file) -> ImageVariant {
        return ustl::visit(
            [](auto const &f) -> ImageVariant {
                using Impl = std::remove_cvref_t<decltype(f)>;
                if constexpr (std::is_same_v<Impl, FileImpl<Traits32>>) {
                    return BasicImage<Traits32>::from(f).value();
                } else {
                    return BasicImage<Traits64>::from(f).value();
                }
            },
            file);
    }

    FileVariant m_file;
    ImageVariant m_image;

  public:
    /// Invokes fn with the underlying typed BasicImage for advanced callers.
    template <typename F>
    [[nodiscard]]
    auto with_impl(F &&fn) const
        -> decltype(std::forward<F>(fn)(std::declval<ImageVariant const &>())) {
        return std::forward<F>(fn)(m_image);
    }

    // ── eftl::Binary interface ──────────────────────────────────────────

    [[nodiscard]]
    auto format() const noexcept -> FormatId override {
        return FormatId::Elf;
    }

    [[nodiscard]]
    auto entrypoint() const noexcept -> u64 override {
        return ustl::visit([](auto const &img) { return img.entrypoint(); }, m_image);
    }

    [[nodiscard]]
    auto imagebase() const noexcept -> u64 override {
        return ustl::visit([](auto const &img) { return img.imagebase(); }, m_image);
    }

    [[nodiscard]]
    auto virtual_address_to_offset(u64 vaddr) const noexcept -> ustl::Option<usize> override {
        return ustl::visit(
            [vaddr](auto const &img) { return img.virtual_address_to_offset(vaddr); }, m_image);
    }

    // ── ELF-specific queries ────────────────────────────────────────────

    [[nodiscard]]
    auto class_type() const noexcept -> Class {
        return ustl::visit(
            [](auto const &img) -> Class {
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(img)>,
                                             BasicImage<Traits32>>) {
                    return Class::Elf32;
                } else {
                    return Class::Elf64;
                }
            },
            m_image);
    }

    [[nodiscard]]
    auto byte_order() const noexcept -> ByteOrder {
        return ustl::visit([](auto const &img) { return img.elf_file().byte_order(); }, m_image);
    }

    [[nodiscard]]
    auto machine() const noexcept -> Arch {
        return ustl::visit(
            [](auto const &img) -> Arch {
                return static_cast<Arch>(img.elf_file().elf_header().e_machine);
            },
            m_image);
    }

    [[nodiscard]]
    auto type() const noexcept -> u16 {
        return ustl::visit(
            [](auto const &img) -> u16 {
                return static_cast<u16>(img.elf_file().elf_header().e_type);
            },
            m_image);
    }

    [[nodiscard]]
    auto segments_count() const noexcept -> usize {
        return ustl::visit([](auto const &img) { return img.elf_file().segments().size(); },
                           m_image);
    }

    [[nodiscard]]
    auto sections_count() const noexcept -> usize {
        return ustl::visit([](auto const &img) { return img.elf_file().sections().size(); },
                           m_image);
    }

    [[nodiscard]]
    auto interpreter_path() const noexcept -> ustl::Option<ByteRange> {
        return ustl::visit([](auto const &img) { return img.elf_file().interpreter_path(); },
                           m_image);
    }

    [[nodiscard]]
    auto section_data(usize index) const noexcept -> ustl::Option<ByteRange> {
        return ustl::visit([index](auto const &img) { return img.elf_file().section_data(index); },
                           m_image);
    }

    [[nodiscard]]
    auto section_name(usize index) const noexcept -> ustl::Option<ByteRange> {
        return ustl::visit([index](auto const &img) { return img.elf_file().section_name(index); },
                           m_image);
    }

    /// Access the underlying typed FileImpl variant for advanced use.
    template <typename F>
    [[nodiscard]]
    auto with_file(F &&fn) const -> decltype(auto) {
        return ustl::visit(std::forward<F>(fn), m_file);
    }
};
} // namespace eftl::elf

#endif // EFTL_ELF_BINARY_HPP
