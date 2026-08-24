/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_ENCODING_HPP
#define EFTL_ELF_ENCODING_HPP 1

#include <eftl/elf/traits.hpp>

#include <bit>
#include <limits>
#include <type_traits>

#include <cstddef>
#include <cstring>

namespace eftl::elf {
enum class ByteOrder {
    Little,
    Big,
};

namespace detail {
    template <typename Value>
    [[nodiscard]]
    constexpr auto decode_scalar(u8 const *data, ByteOrder byte_order) noexcept -> Value {
        static_assert(std::is_unsigned_v<Value>);
        static_assert(std::numeric_limits<u8>::digits == 8);

        auto value = Value{0};
        for (usize index = 0; index < sizeof(Value); ++index) {
            auto byte_index = sizeof(Value) - index - 1;
            if (byte_order == ByteOrder::Little) {
                byte_index = index;
            }
            value |= static_cast<Value>(static_cast<Value>(data[byte_index]) << (index * 8));
        }
        return value;
    }

    template <typename Value>
    [[nodiscard]]
    constexpr auto decode_signed_scalar(u8 const *data, ByteOrder byte_order) noexcept
        -> Value {
        static_assert(std::is_signed_v<Value>);
        using Unsigned = std::make_unsigned_t<Value>;
        return std::bit_cast<Value>(decode_scalar<Unsigned>(data, byte_order));
    }

    template <typename Header>
    [[nodiscard]]
    auto decode_header(u8 const *data, ByteOrder byte_order) noexcept -> Header {
        auto header = Header{};
        std::memcpy(header.e_ident, data + offsetof(Header, e_ident), sizeof(header.e_ident));
        header.e_type =
            decode_scalar<decltype(header.e_type)>(data + offsetof(Header, e_type), byte_order);
        header.e_machine = decode_scalar<decltype(header.e_machine)>(
            data + offsetof(Header, e_machine), byte_order);
        header.e_version = decode_scalar<decltype(header.e_version)>(
            data + offsetof(Header, e_version), byte_order);
        header.e_entry =
            decode_scalar<decltype(header.e_entry)>(data + offsetof(Header, e_entry), byte_order);
        header.e_phoff =
            decode_scalar<decltype(header.e_phoff)>(data + offsetof(Header, e_phoff), byte_order);
        header.e_shoff =
            decode_scalar<decltype(header.e_shoff)>(data + offsetof(Header, e_shoff), byte_order);
        header.e_flags =
            decode_scalar<decltype(header.e_flags)>(data + offsetof(Header, e_flags), byte_order);
        header.e_ehsize =
            decode_scalar<decltype(header.e_ehsize)>(data + offsetof(Header, e_ehsize), byte_order);
        header.e_phentsize = decode_scalar<decltype(header.e_phentsize)>(
            data + offsetof(Header, e_phentsize), byte_order);
        header.e_phnum =
            decode_scalar<decltype(header.e_phnum)>(data + offsetof(Header, e_phnum), byte_order);
        header.e_shentsize = decode_scalar<decltype(header.e_shentsize)>(
            data + offsetof(Header, e_shentsize), byte_order);
        header.e_shnum =
            decode_scalar<decltype(header.e_shnum)>(data + offsetof(Header, e_shnum), byte_order);
        header.e_shstrndx = decode_scalar<decltype(header.e_shstrndx)>(
            data + offsetof(Header, e_shstrndx), byte_order);
        return header;
    }

    template <typename Segment>
    [[nodiscard]]
    auto decode_segment(u8 const *data, ByteOrder byte_order) noexcept -> Segment {
        auto segment = Segment{};
        segment.p_type =
            decode_scalar<decltype(segment.p_type)>(data + offsetof(Segment, p_type), byte_order);
        segment.p_offset = decode_scalar<decltype(segment.p_offset)>(
            data + offsetof(Segment, p_offset), byte_order);
        segment.p_vaddr =
            decode_scalar<decltype(segment.p_vaddr)>(data + offsetof(Segment, p_vaddr), byte_order);
        segment.p_paddr =
            decode_scalar<decltype(segment.p_paddr)>(data + offsetof(Segment, p_paddr), byte_order);
        segment.p_filesz = decode_scalar<decltype(segment.p_filesz)>(
            data + offsetof(Segment, p_filesz), byte_order);
        segment.p_memsz =
            decode_scalar<decltype(segment.p_memsz)>(data + offsetof(Segment, p_memsz), byte_order);
        segment.p_flags =
            decode_scalar<decltype(segment.p_flags)>(data + offsetof(Segment, p_flags), byte_order);
        segment.p_align =
            decode_scalar<decltype(segment.p_align)>(data + offsetof(Segment, p_align), byte_order);
        return segment;
    }

    template <typename Section>
    [[nodiscard]]
    auto decode_section(u8 const *data, ByteOrder byte_order) noexcept -> Section {
        auto section = Section{};
        section.sh_name =
            decode_scalar<decltype(section.sh_name)>(data + offsetof(Section, sh_name), byte_order);
        section.sh_type =
            decode_scalar<decltype(section.sh_type)>(data + offsetof(Section, sh_type), byte_order);
        section.sh_flags = decode_scalar<decltype(section.sh_flags)>(
            data + offsetof(Section, sh_flags), byte_order);
        section.sh_addr =
            decode_scalar<decltype(section.sh_addr)>(data + offsetof(Section, sh_addr), byte_order);
        section.sh_offset = decode_scalar<decltype(section.sh_offset)>(
            data + offsetof(Section, sh_offset), byte_order);
        section.sh_size =
            decode_scalar<decltype(section.sh_size)>(data + offsetof(Section, sh_size), byte_order);
        section.sh_link =
            decode_scalar<decltype(section.sh_link)>(data + offsetof(Section, sh_link), byte_order);
        section.sh_info =
            decode_scalar<decltype(section.sh_info)>(data + offsetof(Section, sh_info), byte_order);
        section.sh_addralign = decode_scalar<decltype(section.sh_addralign)>(
            data + offsetof(Section, sh_addralign), byte_order);
        section.sh_entsize = decode_scalar<decltype(section.sh_entsize)>(
            data + offsetof(Section, sh_entsize), byte_order);
        return section;
    }

    [[nodiscard]]
    inline auto decode_compression_header32(u8 const *data, ByteOrder byte_order) noexcept
        -> Elf32Chdr {
        auto header = Elf32Chdr{};
        header.ch_type = decode_scalar<decltype(header.ch_type)>(
            data + offsetof(Elf32Chdr, ch_type), byte_order);
        header.ch_size = decode_scalar<decltype(header.ch_size)>(
            data + offsetof(Elf32Chdr, ch_size), byte_order);
        header.ch_addralign = decode_scalar<decltype(header.ch_addralign)>(
            data + offsetof(Elf32Chdr, ch_addralign), byte_order);
        return header;
    }

    [[nodiscard]]
    inline auto decode_compression_header64(u8 const *data, ByteOrder byte_order) noexcept
        -> Elf64Chdr {
        auto header = Elf64Chdr{};
        header.ch_type = decode_scalar<decltype(header.ch_type)>(
            data + offsetof(Elf64Chdr, ch_type), byte_order);
        header.ch_reserved = decode_scalar<decltype(header.ch_reserved)>(
            data + offsetof(Elf64Chdr, ch_reserved), byte_order);
        header.ch_size = decode_scalar<decltype(header.ch_size)>(
            data + offsetof(Elf64Chdr, ch_size), byte_order);
        header.ch_addralign = decode_scalar<decltype(header.ch_addralign)>(
            data + offsetof(Elf64Chdr, ch_addralign), byte_order);
        return header;
    }

    [[nodiscard]]
    inline auto decode_note_header(u8 const *data, ByteOrder byte_order) noexcept
        -> ElfNoteHeader {
        auto header = ElfNoteHeader{};
        header.n_namesz = decode_scalar<decltype(header.n_namesz)>(
            data + offsetof(ElfNoteHeader, n_namesz), byte_order);
        header.n_descsz = decode_scalar<decltype(header.n_descsz)>(
            data + offsetof(ElfNoteHeader, n_descsz), byte_order);
        header.n_type = decode_scalar<decltype(header.n_type)>(
            data + offsetof(ElfNoteHeader, n_type), byte_order);
        return header;
    }

    template <typename Dynamic>
    [[nodiscard]]
    auto decode_dynamic(u8 const *data, ByteOrder byte_order) noexcept -> Dynamic {
        auto dynamic = Dynamic{};
        dynamic.d_tag = decode_signed_scalar<decltype(dynamic.d_tag)>(data, byte_order);
        dynamic.d_val =
            decode_scalar<decltype(dynamic.d_val)>(data + sizeof(dynamic.d_tag), byte_order);
        return dynamic;
    }

    [[nodiscard]]
    inline auto decode_symbol32(u8 const *data, ByteOrder byte_order) noexcept -> Elf32Sym {
        auto symbol = Elf32Sym{};
        symbol.st_name =
            decode_scalar<decltype(symbol.st_name)>(data + offsetof(Elf32Sym, st_name), byte_order);
        symbol.st_value = decode_scalar<decltype(symbol.st_value)>(
            data + offsetof(Elf32Sym, st_value), byte_order);
        symbol.st_size =
            decode_scalar<decltype(symbol.st_size)>(data + offsetof(Elf32Sym, st_size), byte_order);
        symbol.st_info = data[offsetof(Elf32Sym, st_info)];
        symbol.st_other = data[offsetof(Elf32Sym, st_other)];
        symbol.st_shndx = decode_scalar<decltype(symbol.st_shndx)>(
            data + offsetof(Elf32Sym, st_shndx), byte_order);
        return symbol;
    }

    [[nodiscard]]
    inline auto decode_symbol64(u8 const *data, ByteOrder byte_order) noexcept -> Elf64Sym {
        auto symbol = Elf64Sym{};
        symbol.st_name =
            decode_scalar<decltype(symbol.st_name)>(data + offsetof(Elf64Sym, st_name), byte_order);
        symbol.st_info = data[offsetof(Elf64Sym, st_info)];
        symbol.st_other = data[offsetof(Elf64Sym, st_other)];
        symbol.st_shndx = decode_scalar<decltype(symbol.st_shndx)>(
            data + offsetof(Elf64Sym, st_shndx), byte_order);
        symbol.st_value = decode_scalar<decltype(symbol.st_value)>(
            data + offsetof(Elf64Sym, st_value), byte_order);
        symbol.st_size =
            decode_scalar<decltype(symbol.st_size)>(data + offsetof(Elf64Sym, st_size), byte_order);
        return symbol;
    }

    [[nodiscard]]
    inline auto decode_rel32(u8 const *data, ByteOrder byte_order) noexcept -> Elf32Rel {
        auto relocation = Elf32Rel{};
        relocation.r_offset = decode_scalar<decltype(relocation.r_offset)>(
            data + offsetof(Elf32Rel, r_offset), byte_order);
        relocation.r_info = decode_scalar<decltype(relocation.r_info)>(
            data + offsetof(Elf32Rel, r_info), byte_order);
        return relocation;
    }

    [[nodiscard]]
    inline auto decode_rela32(u8 const *data, ByteOrder byte_order) noexcept -> Elf32Rela {
        auto relocation = Elf32Rela{};
        relocation.r_offset = decode_scalar<decltype(relocation.r_offset)>(
            data + offsetof(Elf32Rela, r_offset), byte_order);
        relocation.r_info = decode_scalar<decltype(relocation.r_info)>(
            data + offsetof(Elf32Rela, r_info), byte_order);
        relocation.r_addend = decode_signed_scalar<decltype(relocation.r_addend)>(
            data + offsetof(Elf32Rela, r_addend), byte_order);
        return relocation;
    }

    [[nodiscard]]
    inline auto decode_rel64(u8 const *data, ByteOrder byte_order) noexcept -> Elf64Rel {
        auto relocation = Elf64Rel{};
        relocation.r_offset = decode_scalar<decltype(relocation.r_offset)>(
            data + offsetof(Elf64Rel, r_offset), byte_order);
        relocation.r_info = decode_scalar<decltype(relocation.r_info)>(
            data + offsetof(Elf64Rel, r_info), byte_order);
        return relocation;
    }

    [[nodiscard]]
    inline auto decode_rela64(u8 const *data, ByteOrder byte_order) noexcept -> Elf64Rela {
        auto relocation = Elf64Rela{};
        relocation.r_offset = decode_scalar<decltype(relocation.r_offset)>(
            data + offsetof(Elf64Rela, r_offset), byte_order);
        relocation.r_info = decode_scalar<decltype(relocation.r_info)>(
            data + offsetof(Elf64Rela, r_info), byte_order);
        relocation.r_addend = decode_signed_scalar<decltype(relocation.r_addend)>(
            data + offsetof(Elf64Rela, r_addend), byte_order);
        return relocation;
    }

    template <typename Definition>
    [[nodiscard]]
    auto decode_version_definition(u8 const *data, ByteOrder byte_order) noexcept
        -> Definition {
        auto definition = Definition{};
        definition.vd_version = decode_scalar<decltype(definition.vd_version)>(
            data + offsetof(Definition, vd_version), byte_order);
        definition.vd_flags = decode_scalar<decltype(definition.vd_flags)>(
            data + offsetof(Definition, vd_flags), byte_order);
        definition.vd_ndx = decode_scalar<decltype(definition.vd_ndx)>(
            data + offsetof(Definition, vd_ndx), byte_order);
        definition.vd_cnt = decode_scalar<decltype(definition.vd_cnt)>(
            data + offsetof(Definition, vd_cnt), byte_order);
        definition.vd_hash = decode_scalar<decltype(definition.vd_hash)>(
            data + offsetof(Definition, vd_hash), byte_order);
        definition.vd_aux = decode_scalar<decltype(definition.vd_aux)>(
            data + offsetof(Definition, vd_aux), byte_order);
        definition.vd_next = decode_scalar<decltype(definition.vd_next)>(
            data + offsetof(Definition, vd_next), byte_order);
        return definition;
    }

    template <typename Auxiliary>
    [[nodiscard]]
    auto decode_version_definition_auxiliary(u8 const *data, ByteOrder byte_order) noexcept
        -> Auxiliary {
        auto auxiliary = Auxiliary{};
        auxiliary.vda_name = decode_scalar<decltype(auxiliary.vda_name)>(
            data + offsetof(Auxiliary, vda_name), byte_order);
        auxiliary.vda_next = decode_scalar<decltype(auxiliary.vda_next)>(
            data + offsetof(Auxiliary, vda_next), byte_order);
        return auxiliary;
    }

    template <typename Requirement>
    [[nodiscard]]
    auto decode_version_requirement(u8 const *data, ByteOrder byte_order) noexcept
        -> Requirement {
        auto requirement = Requirement{};
        requirement.vn_version = decode_scalar<decltype(requirement.vn_version)>(
            data + offsetof(Requirement, vn_version), byte_order);
        requirement.vn_cnt = decode_scalar<decltype(requirement.vn_cnt)>(
            data + offsetof(Requirement, vn_cnt), byte_order);
        requirement.vn_file = decode_scalar<decltype(requirement.vn_file)>(
            data + offsetof(Requirement, vn_file), byte_order);
        requirement.vn_aux = decode_scalar<decltype(requirement.vn_aux)>(
            data + offsetof(Requirement, vn_aux), byte_order);
        requirement.vn_next = decode_scalar<decltype(requirement.vn_next)>(
            data + offsetof(Requirement, vn_next), byte_order);
        return requirement;
    }

    template <typename Auxiliary>
    [[nodiscard]]
    auto decode_version_requirement_auxiliary(u8 const *data, ByteOrder byte_order) noexcept
        -> Auxiliary {
        auto auxiliary = Auxiliary{};
        auxiliary.vna_hash = decode_scalar<decltype(auxiliary.vna_hash)>(
            data + offsetof(Auxiliary, vna_hash), byte_order);
        auxiliary.vna_flags = decode_scalar<decltype(auxiliary.vna_flags)>(
            data + offsetof(Auxiliary, vna_flags), byte_order);
        auxiliary.vna_other = decode_scalar<decltype(auxiliary.vna_other)>(
            data + offsetof(Auxiliary, vna_other), byte_order);
        auxiliary.vna_name = decode_scalar<decltype(auxiliary.vna_name)>(
            data + offsetof(Auxiliary, vna_name), byte_order);
        auxiliary.vna_next = decode_scalar<decltype(auxiliary.vna_next)>(
            data + offsetof(Auxiliary, vna_next), byte_order);
        return auxiliary;
    }
} // namespace detail

template <typename Traits, typename Record>
struct RecordCodec;

template <typename Value>
struct ScalarCodec final {
    static_assert(std::is_unsigned_v<Value>);

    [[nodiscard]]
    static constexpr auto decode_little(u8 const *data) noexcept -> Value {
        return detail::decode_scalar<Value>(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static constexpr auto decode_big(u8 const *data) noexcept -> Value {
        return detail::decode_scalar<Value>(data, ByteOrder::Big);
    }
};

template <typename Traits>
struct RecordCodec<Traits, typename Traits::Header> final {
    using Record = typename Traits::Header;

    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Record {
        return detail::decode_header<Record>(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Record {
        return detail::decode_header<Record>(data, ByteOrder::Big);
    }
};

template <typename Traits>
struct RecordCodec<Traits, typename Traits::Segment> final {
    using Record = typename Traits::Segment;

    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Record {
        return detail::decode_segment<Record>(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Record {
        return detail::decode_segment<Record>(data, ByteOrder::Big);
    }
};

template <typename Traits>
struct RecordCodec<Traits, typename Traits::Section> final {
    using Record = typename Traits::Section;

    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Record {
        return detail::decode_section<Record>(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Record {
        return detail::decode_section<Record>(data, ByteOrder::Big);
    }
};

template <>
struct RecordCodec<Traits32, Elf32Chdr> final {
    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Elf32Chdr {
        return detail::decode_compression_header32(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Elf32Chdr {
        return detail::decode_compression_header32(data, ByteOrder::Big);
    }
};

template <>
struct RecordCodec<Traits64, Elf64Chdr> final {
    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Elf64Chdr {
        return detail::decode_compression_header64(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Elf64Chdr {
        return detail::decode_compression_header64(data, ByteOrder::Big);
    }
};

template <typename Traits>
struct RecordCodec<Traits, ElfNoteHeader> final {
    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> ElfNoteHeader {
        return detail::decode_note_header(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> ElfNoteHeader {
        return detail::decode_note_header(data, ByteOrder::Big);
    }
};

template <typename Traits>
struct RecordCodec<Traits, typename Traits::Dyn> final {
    using Record = typename Traits::Dyn;

    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Record {
        return detail::decode_dynamic<Record>(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Record {
        return detail::decode_dynamic<Record>(data, ByteOrder::Big);
    }
};

template <>
struct RecordCodec<Traits32, Elf32Sym> final {
    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Elf32Sym {
        return detail::decode_symbol32(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Elf32Sym {
        return detail::decode_symbol32(data, ByteOrder::Big);
    }
};

template <>
struct RecordCodec<Traits64, Elf64Sym> final {
    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Elf64Sym {
        return detail::decode_symbol64(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Elf64Sym {
        return detail::decode_symbol64(data, ByteOrder::Big);
    }
};

template <>
struct RecordCodec<Traits32, Elf32Rel> final {
    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Elf32Rel {
        return detail::decode_rel32(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Elf32Rel {
        return detail::decode_rel32(data, ByteOrder::Big);
    }
};

template <>
struct RecordCodec<Traits32, Elf32Rela> final {
    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Elf32Rela {
        return detail::decode_rela32(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Elf32Rela {
        return detail::decode_rela32(data, ByteOrder::Big);
    }
};

template <>
struct RecordCodec<Traits64, Elf64Rel> final {
    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Elf64Rel {
        return detail::decode_rel64(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Elf64Rel {
        return detail::decode_rel64(data, ByteOrder::Big);
    }
};

template <>
struct RecordCodec<Traits64, Elf64Rela> final {
    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Elf64Rela {
        return detail::decode_rela64(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Elf64Rela {
        return detail::decode_rela64(data, ByteOrder::Big);
    }
};

template <typename Traits>
struct RecordCodec<Traits, typename Traits::Verdef> final {
    using Record = typename Traits::Verdef;

    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Record {
        return detail::decode_version_definition<Record>(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Record {
        return detail::decode_version_definition<Record>(data, ByteOrder::Big);
    }
};

template <typename Traits>
struct RecordCodec<Traits, typename Traits::Verdaux> final {
    using Record = typename Traits::Verdaux;

    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Record {
        return detail::decode_version_definition_auxiliary<Record>(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Record {
        return detail::decode_version_definition_auxiliary<Record>(data, ByteOrder::Big);
    }
};

template <typename Traits>
struct RecordCodec<Traits, typename Traits::Verneed> final {
    using Record = typename Traits::Verneed;

    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Record {
        return detail::decode_version_requirement<Record>(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Record {
        return detail::decode_version_requirement<Record>(data, ByteOrder::Big);
    }
};

template <typename Traits>
struct RecordCodec<Traits, typename Traits::Vernaux> final {
    using Record = typename Traits::Vernaux;

    [[nodiscard]]
    static auto decode_little(u8 const *data) noexcept -> Record {
        return detail::decode_version_requirement_auxiliary<Record>(data, ByteOrder::Little);
    }

    [[nodiscard]]
    static auto decode_big(u8 const *data) noexcept -> Record {
        return detail::decode_version_requirement_auxiliary<Record>(data, ByteOrder::Big);
    }
};
} // namespace eftl::elf

#endif // EFTL_ELF_ENCODING_HPP
