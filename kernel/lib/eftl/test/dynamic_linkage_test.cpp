/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/dynamic_array.hpp>
#include <eftl/elf/dynamic_image.hpp>

#include <array>
#include <type_traits>

#include <cstddef>
#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using File = eftl::elf::FileImpl<Traits>;
using Definition = Traits::Verdef;
using DefinitionAuxiliary = Traits::Verdaux;
using Dynamic = Traits::Dyn;
using Header = Traits::Header;
using Requirement = Traits::Verneed;
using RequirementAuxiliary = Traits::Vernaux;
using Segment = Traits::Segment;
using Symbol = Traits::Symbol;
using Version = Traits::Half;
using DynamicImage = eftl::elf::DynamicImageView<Traits>;
using DynamicLinkage = eftl::elf::DynamicLinkageView<Traits>;
using FunctionArray = eftl::elf::DynamicFunctionArrayView<Traits>;
using GnuHash = eftl::elf::GnuHashTableView<Traits>;
using Symbols = eftl::elf::SymbolTableView<Traits>;
using SysvHash = eftl::elf::SysvHashTableView<Traits>;
using VersionBinding = eftl::elf::VersionBinding<Traits>;
using VersionDefinitions = eftl::elf::VersionDefinitionTableView<Traits>;
using VersionRequirements = eftl::elf::VersionRequirementTableView<Traits>;
using VersionSymbols = eftl::elf::VersionSymbolTableView<Traits>;

constexpr usize PROGRAM_HEADERS_OFFSET = sizeof(Header);
constexpr usize LOAD_OFFSET = 0x300;
constexpr u64 LOAD_ADDRESS = 0x400000;
constexpr usize DYNAMIC_OFFSET = LOAD_OFFSET + 0x40;
constexpr u64 DYNAMIC_ADDRESS = LOAD_ADDRESS + 0x40;
constexpr usize DYNAMIC_COUNT = 15;
constexpr usize STRING_OFFSET = LOAD_OFFSET + 0x150;
constexpr u64 STRING_ADDRESS = LOAD_ADDRESS + 0x150;
constexpr usize SYMBOL_OFFSET = LOAD_OFFSET + 0x190;
constexpr u64 SYMBOL_ADDRESS = LOAD_ADDRESS + 0x190;
constexpr usize HASH_OFFSET = LOAD_OFFSET + 0x1e0;
constexpr u64 HASH_ADDRESS = LOAD_ADDRESS + 0x1e0;
constexpr usize VERSYM_OFFSET = DYNAMIC_OFFSET + DYNAMIC_COUNT * sizeof(Dynamic);
constexpr u64 VERSYM_ADDRESS = LOAD_ADDRESS + VERSYM_OFFSET - LOAD_OFFSET;
constexpr usize VERDEF_OFFSET = LOAD_OFFSET + 0x220;
constexpr u64 VERDEF_ADDRESS = LOAD_ADDRESS + 0x220;
constexpr usize VERNEED_OFFSET = LOAD_OFFSET + 0x240;
constexpr u64 VERNEED_ADDRESS = LOAD_ADDRESS + 0x240;
constexpr usize INIT_ARRAY_OFFSET = LOAD_OFFSET + 0x280;
constexpr u64 INIT_ARRAY_ADDRESS = LOAD_ADDRESS + 0x280;
constexpr usize FINI_ARRAY_OFFSET = LOAD_OFFSET + 0x298;
constexpr u64 FINI_ARRAY_ADDRESS = LOAD_ADDRESS + 0x298;
constexpr usize PREINIT_ARRAY_OFFSET = LOAD_OFFSET + 0x2a8;
constexpr u64 PREINIT_ARRAY_ADDRESS = LOAD_ADDRESS + 0x2a8;

template <typename Value, usize Size>
auto put(std::array<u8, Size> &image, usize offset, Value const &value) -> void {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

template <typename Value, usize Size>
auto put_scalar(std::array<u8, Size> &image, usize offset, Value value, bool big_endian) -> void {
    static_assert(std::is_unsigned_v<Value>);
    if (!big_endian) {
        put(image, offset, value);
        return;
    }
    for (usize index = 0; index < sizeof(Value); ++index) {
        auto const shift = (sizeof(Value) - index - 1) * 8U;
        image[offset + index] = static_cast<u8>(value >> shift);
    }
}

template <usize Size>
auto put_header(std::array<u8, Size> &image, Header const &header, bool big_endian) -> void {
    std::memcpy(image.data(), header.e_ident, sizeof(header.e_ident));
    put_scalar(image, offsetof(Header, e_type), header.e_type, big_endian);
    put_scalar(image, offsetof(Header, e_machine), header.e_machine, big_endian);
    put_scalar(image, offsetof(Header, e_version), header.e_version, big_endian);
    put_scalar(image, offsetof(Header, e_entry), header.e_entry, big_endian);
    put_scalar(image, offsetof(Header, e_phoff), header.e_phoff, big_endian);
    put_scalar(image, offsetof(Header, e_shoff), header.e_shoff, big_endian);
    put_scalar(image, offsetof(Header, e_flags), header.e_flags, big_endian);
    put_scalar(image, offsetof(Header, e_ehsize), header.e_ehsize, big_endian);
    put_scalar(image, offsetof(Header, e_phentsize), header.e_phentsize, big_endian);
    put_scalar(image, offsetof(Header, e_phnum), header.e_phnum, big_endian);
    put_scalar(image, offsetof(Header, e_shentsize), header.e_shentsize, big_endian);
    put_scalar(image, offsetof(Header, e_shnum), header.e_shnum, big_endian);
    put_scalar(image, offsetof(Header, e_shstrndx), header.e_shstrndx, big_endian);
}

template <usize Size>
auto put_segment(std::array<u8, Size> &image, usize offset, Segment const &segment, bool big_endian)
    -> void {
    put_scalar(image, offset + offsetof(Segment, p_type), segment.p_type, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_flags), segment.p_flags, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_offset), segment.p_offset, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_vaddr), segment.p_vaddr, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_paddr), segment.p_paddr, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_filesz), segment.p_filesz, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_memsz), segment.p_memsz, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_align), segment.p_align, big_endian);
}

template <usize Size>
auto put_dynamic(std::array<u8, Size> &image, usize offset, Dynamic const &dynamic, bool big_endian)
    -> void {
    put_scalar(image, offset + offsetof(Dynamic, d_tag), static_cast<u64>(dynamic.d_tag),
               big_endian);
    put_scalar(image, offset + offsetof(Dynamic, d_val), dynamic.d_val, big_endian);
}

template <usize Size>
auto put_symbol(std::array<u8, Size> &image, usize offset, Symbol const &symbol, bool big_endian)
    -> void {
    put_scalar(image, offset + offsetof(Symbol, st_name), symbol.st_name, big_endian);
    image[offset + offsetof(Symbol, st_info)] = symbol.st_info;
    image[offset + offsetof(Symbol, st_other)] = symbol.st_other;
    put_scalar(image, offset + offsetof(Symbol, st_shndx), symbol.st_shndx, big_endian);
    put_scalar(image, offset + offsetof(Symbol, st_value), symbol.st_value, big_endian);
    put_scalar(image, offset + offsetof(Symbol, st_size), symbol.st_size, big_endian);
}

template <usize Size>
auto put_definition(std::array<u8, Size> &image, usize offset, Definition const &definition,
                    bool big_endian) -> void {
    put_scalar(image, offset + offsetof(Definition, vd_version), definition.vd_version, big_endian);
    put_scalar(image, offset + offsetof(Definition, vd_flags), definition.vd_flags, big_endian);
    put_scalar(image, offset + offsetof(Definition, vd_ndx), definition.vd_ndx, big_endian);
    put_scalar(image, offset + offsetof(Definition, vd_cnt), definition.vd_cnt, big_endian);
    put_scalar(image, offset + offsetof(Definition, vd_hash), definition.vd_hash, big_endian);
    put_scalar(image, offset + offsetof(Definition, vd_aux), definition.vd_aux, big_endian);
    put_scalar(image, offset + offsetof(Definition, vd_next), definition.vd_next, big_endian);
}

template <usize Size>
auto put_definition_auxiliary(std::array<u8, Size> &image, usize offset,
                              DefinitionAuxiliary const &auxiliary, bool big_endian) -> void {
    put_scalar(image, offset + offsetof(DefinitionAuxiliary, vda_name), auxiliary.vda_name,
               big_endian);
    put_scalar(image, offset + offsetof(DefinitionAuxiliary, vda_next), auxiliary.vda_next,
               big_endian);
}

template <usize Size>
auto put_requirement(std::array<u8, Size> &image, usize offset, Requirement const &requirement,
                     bool big_endian) -> void {
    put_scalar(image, offset + offsetof(Requirement, vn_version), requirement.vn_version,
               big_endian);
    put_scalar(image, offset + offsetof(Requirement, vn_cnt), requirement.vn_cnt, big_endian);
    put_scalar(image, offset + offsetof(Requirement, vn_file), requirement.vn_file, big_endian);
    put_scalar(image, offset + offsetof(Requirement, vn_aux), requirement.vn_aux, big_endian);
    put_scalar(image, offset + offsetof(Requirement, vn_next), requirement.vn_next, big_endian);
}

template <usize Size>
auto put_requirement_auxiliary(std::array<u8, Size> &image, usize offset,
                               RequirementAuxiliary const &auxiliary, bool big_endian) -> void {
    put_scalar(image, offset + offsetof(RequirementAuxiliary, vna_hash), auxiliary.vna_hash,
               big_endian);
    put_scalar(image, offset + offsetof(RequirementAuxiliary, vna_flags), auxiliary.vna_flags,
               big_endian);
    put_scalar(image, offset + offsetof(RequirementAuxiliary, vna_other), auxiliary.vna_other,
               big_endian);
    put_scalar(image, offset + offsetof(RequirementAuxiliary, vna_name), auxiliary.vna_name,
               big_endian);
    put_scalar(image, offset + offsetof(RequirementAuxiliary, vna_next), auxiliary.vna_next,
               big_endian);
}

auto make_linkage_image(bool big_endian = false, bool gnu_hash = false, bool versions = false,
                        bool metadata = false) -> std::array<u8, 2048> {
    std::array<u8, 2048> image{};
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    header.e_ident[4] = Traits::ELF_CLASS;
    if (big_endian) {
        header.e_ident[5] = 2;
    } else {
        header.e_ident[5] = 1;
    }
    header.e_ident[6] = 1;
    header.e_type = 3;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    header.e_phoff = PROGRAM_HEADERS_OFFSET;
    header.e_phentsize = sizeof(Segment);
    header.e_phnum = 2;
    put_header(image, header, big_endian);

    Segment load{};
    load.p_type = static_cast<u32>(eftl::elf::SegType::Load);
    load.p_offset = LOAD_OFFSET;
    load.p_vaddr = LOAD_ADDRESS;
    load.p_filesz = 0x300;
    load.p_memsz = 0x300;
    load.p_align = 0x10;
    put_segment(image, PROGRAM_HEADERS_OFFSET, load, big_endian);

    Segment dynamic{};
    dynamic.p_type = static_cast<u32>(eftl::elf::SegType::Dynamic);
    dynamic.p_offset = DYNAMIC_OFFSET;
    dynamic.p_vaddr = DYNAMIC_ADDRESS;
    dynamic.p_filesz = DYNAMIC_COUNT * sizeof(Dynamic);
    dynamic.p_memsz = dynamic.p_filesz;
    dynamic.p_align = alignof(Dynamic);
    put_segment(image, PROGRAM_HEADERS_OFFSET + sizeof(Segment), dynamic, big_endian);

    std::array<Dynamic, DYNAMIC_COUNT> entries{};
    entries[0] = Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::StrTab),
                         .d_val = STRING_ADDRESS};
    u64 string_size = 9U;
    if (versions) {
        string_size = 35U;
    } else if (metadata) {
        string_size = 59U;
    }
    entries[1] = Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::StrSz),
                         .d_val = string_size};
    entries[2] = Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::SymTab),
                         .d_val = SYMBOL_ADDRESS};
    entries[3] = Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Syment),
                         .d_val = sizeof(Symbol)};
    Traits::Sword hash_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Hash);
    if (gnu_hash) {
        hash_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::GnuHash);
    }
    entries[4] = Dynamic{.d_tag = hash_tag, .d_val = HASH_ADDRESS};
    if (versions) {
        entries[5] = Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::VerSym),
                             .d_val = VERSYM_ADDRESS};
        entries[6] = Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::VerDef),
                             .d_val = VERDEF_ADDRESS};
        entries[7] =
            Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::VerDefNum), .d_val = 1};
        entries[8] = Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::VerNeed),
                             .d_val = VERNEED_ADDRESS};
        entries[9] =
            Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::VerNeedNum), .d_val = 1};
    } else if (metadata) {
        entries[5] =
            Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Needed), .d_val = 1};
        entries[6] =
            Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Needed), .d_val = 13};
        entries[7] =
            Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::SoName), .d_val = 24};
        entries[8] =
            Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Rpath), .d_val = 35};
        entries[9] =
            Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::RunPath), .d_val = 47};
        entries[10] =
            Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Flags), .d_val = 0x5};
        entries[11] =
            Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Flags1), .d_val = 0x9};
    }
    for (usize index = 0; index < entries.size(); ++index) {
        put_dynamic(image, DYNAMIC_OFFSET + index * sizeof(Dynamic), entries[index], big_endian);
    }

    if (versions) {
        std::array<u8, 35> const strings{
            0,   'f', 'o', 'o', 0,   'b', 'a', 'r', 0, 'V', 'E', 'R', '_', 'D', 'E', 'F', 0, 'l',
            'i', 'b', 'd', 'e', 'p', '.', 's', 'o', 0, 'V', 'E', 'R', '_', 'R', 'E', 'Q', 0,
        };
        std::memcpy(image.data() + STRING_OFFSET, strings.data(), strings.size());
    } else if (metadata) {
        std::array<u8, 59> const strings{
            0,   'l', 'i', 'b', 'a', 'l', 'p', 'h', 'a', '.', 's', 'o', 0,   'l', 'i',
            'b', 'b', 'e', 't', 'a', '.', 's', 'o', 0,   'l', 'i', 'b', 's', 'e', 'l',
            'f', '.', 's', 'o', 0,   '/', 'l', 'e', 'g', 'a', 'c', 'y', '/', 'l', 'i',
            'b', 0,   '/', 'm', 'o', 'd', 'e', 'r', 'n', '/', 'l', 'i', 'b', 0,
        };
        std::memcpy(image.data() + STRING_OFFSET, strings.data(), strings.size());
    } else {
        std::array<u8, 9> const strings{0, 'f', 'o', 'o', 0, 'b', 'a', 'r', 0};
        std::memcpy(image.data() + STRING_OFFSET, strings.data(), strings.size());
    }
    Symbol foo{};
    foo.st_name = 1;
    put_symbol(image, SYMBOL_OFFSET + sizeof(Symbol), foo, big_endian);
    Symbol bar{};
    bar.st_name = 5;
    put_symbol(image, SYMBOL_OFFSET + 2 * sizeof(Symbol), bar, big_endian);

    if (!gnu_hash) {
        std::array<u32, 6> const hash_words{1, 3, 1, 0, 2, 0};
        for (usize index = 0; index < hash_words.size(); ++index) {
            put_scalar(image, HASH_OFFSET + index * sizeof(u32), hash_words[index], big_endian);
        }
    } else {
        std::array<u32, 4> const gnu_header{1, 1, 1, 5};
        for (usize index = 0; index < gnu_header.size(); ++index) {
            put_scalar(image, HASH_OFFSET + index * sizeof(u32), gnu_header[index], big_endian);
        }
        constexpr u64 BLOOM = 0x0400000010000220ULL;
        put_scalar(image, HASH_OFFSET + 4 * sizeof(u32), BLOOM, big_endian);
        constexpr u32 BUCKET = 1;
        put_scalar(image, HASH_OFFSET + 4 * sizeof(u32) + sizeof(Traits::Addr), BUCKET, big_endian);
        constexpr u32 FOO_CHAIN = 0x0b887388U;
        constexpr u32 BAR_CHAIN = 0x0b8860bbU;
        put_scalar(image, HASH_OFFSET + 5 * sizeof(u32) + sizeof(Traits::Addr), FOO_CHAIN,
                   big_endian);
        put_scalar(image, HASH_OFFSET + 6 * sizeof(u32) + sizeof(Traits::Addr), BAR_CHAIN,
                   big_endian);
    }
    if (versions) {
        std::array<Version, 3> const symbol_versions{0, 2, 0x8003U};
        for (usize index = 0; index < symbol_versions.size(); ++index) {
            put_scalar(image, VERSYM_OFFSET + index * sizeof(Version), symbol_versions[index],
                       big_endian);
        }
        Definition definition{};
        definition.vd_version = 1;
        definition.vd_ndx = 2;
        definition.vd_cnt = 1;
        definition.vd_aux = sizeof(Definition);
        put_definition(image, VERDEF_OFFSET, definition, big_endian);
        DefinitionAuxiliary definition_name{};
        definition_name.vda_name = 9;
        put_definition_auxiliary(image, VERDEF_OFFSET + sizeof(Definition), definition_name,
                                 big_endian);

        Requirement requirement{};
        requirement.vn_version = 1;
        requirement.vn_cnt = 1;
        requirement.vn_file = 17;
        requirement.vn_aux = sizeof(Requirement);
        put_requirement(image, VERNEED_OFFSET, requirement, big_endian);
        RequirementAuxiliary requirement_name{};
        requirement_name.vna_other = 3;
        requirement_name.vna_name = 27;
        put_requirement_auxiliary(image, VERNEED_OFFSET + sizeof(Requirement), requirement_name,
                                  big_endian);
    }
    return image;
}

auto make_function_array_image(bool big_endian = false) -> std::array<u8, 2048> {
    auto image = make_linkage_image(big_endian);
    Dynamic init_array{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::InitArray),
                       .d_val = INIT_ARRAY_ADDRESS};
    Dynamic init_array_size{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::InitArraySz),
                            .d_val = 3 * sizeof(Traits::Addr)};
    Dynamic fini_array{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::FiniArray),
                       .d_val = FINI_ARRAY_ADDRESS};
    Dynamic fini_array_size{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::FiniArraySz),
                            .d_val = 2 * sizeof(Traits::Addr)};
    Dynamic preinit_array{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::PreinitArray),
                          .d_val = PREINIT_ARRAY_ADDRESS};
    Dynamic preinit_array_size{.d_tag =
                                   static_cast<Traits::Sword>(eftl::elf::DynTag::PreinitArraySz),
                               .d_val = sizeof(Traits::Addr)};
    Dynamic init{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Init),
                 .d_val = LOAD_ADDRESS + 0x281};
    Dynamic fini{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Fini),
                 .d_val = LOAD_ADDRESS + 0x299};
    put_dynamic(image, DYNAMIC_OFFSET + 5 * sizeof(Dynamic), init_array, big_endian);
    put_dynamic(image, DYNAMIC_OFFSET + 6 * sizeof(Dynamic), init_array_size, big_endian);
    put_dynamic(image, DYNAMIC_OFFSET + 7 * sizeof(Dynamic), fini_array, big_endian);
    put_dynamic(image, DYNAMIC_OFFSET + 8 * sizeof(Dynamic), fini_array_size, big_endian);
    put_dynamic(image, DYNAMIC_OFFSET + 9 * sizeof(Dynamic), preinit_array, big_endian);
    put_dynamic(image, DYNAMIC_OFFSET + 10 * sizeof(Dynamic), preinit_array_size, big_endian);
    put_dynamic(image, DYNAMIC_OFFSET + 11 * sizeof(Dynamic), init, big_endian);
    put_dynamic(image, DYNAMIC_OFFSET + 12 * sizeof(Dynamic), fini, big_endian);

    std::array<Traits::Addr, 3> const init_values{0x401001, 0x401101, 0x401201};
    std::array<Traits::Addr, 2> const fini_values{0x402001, 0x402101};
    for (usize index = 0; index < init_values.size(); ++index) {
        put_scalar(image, INIT_ARRAY_OFFSET + index * sizeof(Traits::Addr), init_values[index],
                   big_endian);
    }
    for (usize index = 0; index < fini_values.size(); ++index) {
        put_scalar(image, FINI_ARRAY_OFFSET + index * sizeof(Traits::Addr), fini_values[index],
                   big_endian);
    }
    put_scalar(image, PREINIT_ARRAY_OFFSET, static_cast<Traits::Addr>(0x403001), big_endian);
    return image;
}

auto test_discovers_dynamic_strings_symbols_and_sysv_hash() -> int {
    auto const bytes = make_linkage_image();
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    ustl::Option<DynamicImage> image;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    } else {
        image = ustl::NONE;
    }
    ustl::Option<ByteRange> strings;
    if (image.is_some()) {
        strings = image->dynamic_strings();
    } else {
        strings = ustl::NONE;
    }
    ustl::Option<Symbols> symbols;
    if (image.is_some()) {
        symbols = image->dynamic_symbols();
    } else {
        symbols = ustl::NONE;
    }
    ustl::Option<SysvHash> hash;
    if (image.is_some()) {
        hash = image->sysv_hash_table();
    } else {
        hash = ustl::NONE;
    }
    std::array<u8, 3> const foo{'f', 'o', 'o'};
    std::array<u8, 3> const bar{'b', 'a', 'r'};
    ustl::Option<usize> foo_index;
    if (hash.is_some()) {
        foo_index = hash->lookup(eftl::ByteRange(foo.data(), foo.size()));
    } else {
        foo_index = ustl::NONE;
    }
    ustl::Option<usize> bar_index;
    if (hash.is_some()) {
        bar_index = hash->lookup(eftl::ByteRange(bar.data(), bar.size()));
    } else {
        bar_index = ustl::NONE;
    }
    ustl::Option<ByteRange> foo_name;
    if (symbols.is_some()) {
        foo_name = symbols->symbol_name(1);
    } else {
        foo_name = ustl::NONE;
    }
    if (strings.is_some() && strings->size() == 9 && symbols.is_some() && symbols->size() == 3 &&
        foo_name.is_some() && foo_name->size() == 3 && foo_name->data()[0] == 'f' &&
        hash.is_some() && hash->chain_count() == 3 && foo_index.is_some() &&
        foo_index.value() == 1 && bar_index.is_some() && bar_index.value() == 2) {
        return 0;
    }
    return 1;
}

auto test_discovers_big_endian_dynamic_linkage() -> int {
    auto const bytes = make_linkage_image(true);
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    ustl::Option<DynamicImage> image;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    } else {
        image = ustl::NONE;
    }
    ustl::Option<Symbols> symbols;
    if (image.is_some()) {
        symbols = image->dynamic_symbols();
    } else {
        symbols = ustl::NONE;
    }
    ustl::Option<SysvHash> hash;
    if (image.is_some()) {
        hash = image->sysv_hash_table();
    } else {
        hash = ustl::NONE;
    }
    std::array<u8, 3> const bar{'b', 'a', 'r'};
    ustl::Option<usize> bar_index;
    if (hash.is_some()) {
        bar_index = hash->lookup(eftl::ByteRange(bar.data(), bar.size()));
    } else {
        bar_index = ustl::NONE;
    }
    ustl::Option<ByteRange> bar_name;
    if (symbols.is_some()) {
        bar_name = symbols->symbol_name(2);
    } else {
        bar_name = ustl::NONE;
    }
    if (symbols.is_some() && symbols->size() == 3 && bar_name.is_some() && bar_name->size() == 3 &&
        bar_name->data()[0] == 'b' && hash.is_some() && bar_index.is_some() &&
        bar_index.value() == 2) {
        return 0;
    }
    return 1;
}

auto test_discovers_dynamic_gnu_hash() -> int {
    auto const bytes = make_linkage_image(false, true);
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    ustl::Option<DynamicImage> image;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    } else {
        image = ustl::NONE;
    }
    ustl::Option<Symbols> symbols;
    if (image.is_some()) {
        symbols = image->dynamic_symbols();
    } else {
        symbols = ustl::NONE;
    }
    ustl::Option<GnuHash> hash;
    if (image.is_some()) {
        hash = image->gnu_hash_table();
    } else {
        hash = ustl::NONE;
    }
    std::array<u8, 3> const foo{'f', 'o', 'o'};
    ustl::Option<usize> foo_index;
    if (hash.is_some()) {
        foo_index = hash->lookup(eftl::ByteRange(foo.data(), foo.size()));
    } else {
        foo_index = ustl::NONE;
    }
    if (symbols.is_some() && symbols->size() == 3 && hash.is_some() && hash->symbol_offset() == 1 &&
        hash->chain_count() == 2 && foo_index.is_some() && foo_index.value() == 1) {
        return 0;
    }
    return 1;
}

auto test_discovers_big_endian_dynamic_gnu_hash() -> int {
    auto const bytes = make_linkage_image(true, true);
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    ustl::Option<DynamicImage> image;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    } else {
        image = ustl::NONE;
    }
    ustl::Option<GnuHash> hash;
    if (image.is_some()) {
        hash = image->gnu_hash_table();
    } else {
        hash = ustl::NONE;
    }
    std::array<u8, 3> const bar{'b', 'a', 'r'};
    ustl::Option<usize> bar_index;
    if (hash.is_some()) {
        bar_index = hash->lookup(eftl::ByteRange(bar.data(), bar.size()));
    } else {
        bar_index = ustl::NONE;
    }
    if (hash.is_some() && hash->bloom_at(0).is_some() &&
        hash->bloom_at(0).value() == 0x0400000010000220ULL && bar_index.is_some() &&
        bar_index.value() == 2) {
        return 0;
    }
    return 1;
}

auto equals(eftl::ByteRange value, char const *text, usize size) -> bool {
    return value.size() == size && std::memcmp(value.data(), text, size) == 0;
}

auto test_observes_ordered_dynamic_linkage_metadata(bool big_endian) -> int {
    auto const bytes = make_linkage_image(big_endian, false, false, true);
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    ustl::Option<DynamicImage> image;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    } else {
        image = ustl::NONE;
    }
    ustl::Option<DynamicLinkage> linkage;
    if (image.is_some()) {
        linkage = image->linkage();
    } else {
        linkage = ustl::NONE;
    }
    ustl::Option<ByteRange> first_needed;
    if (linkage.is_some()) {
        first_needed = linkage->needed_library_at(0);
    } else {
        first_needed = ustl::NONE;
    }
    ustl::Option<ByteRange> second_needed;
    if (linkage.is_some()) {
        second_needed = linkage->needed_library_at(1);
    } else {
        second_needed = ustl::NONE;
    }
    ustl::Option<ByteRange> soname;
    if (linkage.is_some()) {
        soname = linkage->soname();
    } else {
        soname = ustl::NONE;
    }
    ustl::Option<ByteRange> rpath;
    if (linkage.is_some()) {
        rpath = linkage->rpath();
    } else {
        rpath = ustl::NONE;
    }
    ustl::Option<ByteRange> runpath;
    if (linkage.is_some()) {
        runpath = linkage->runpath();
    } else {
        runpath = ustl::NONE;
    }
    ustl::Option<u64> flags;
    if (linkage.is_some()) {
        flags = linkage->flags();
    } else {
        flags = ustl::NONE;
    }
    ustl::Option<u64> flags1;
    if (linkage.is_some()) {
        flags1 = linkage->flags1();
    } else {
        flags1 = ustl::NONE;
    }
    if (linkage.is_some() && linkage->needed_library_count() == 2 && first_needed.is_some() &&
        equals(first_needed.value(), "libalpha.so", 11) && second_needed.is_some() &&
        equals(second_needed.value(), "libbeta.so", 10) && soname.is_some() &&
        equals(soname.value(), "libself.so", 10) && rpath.is_some() &&
        equals(rpath.value(), "/legacy/lib", 11) && runpath.is_some() &&
        equals(runpath.value(), "/modern/lib", 11) && flags.is_some() && flags.value() == 0x5 &&
        flags1.is_some() && flags1.value() == 0x9 && linkage->needed_library_at(2).is_none()) {
        return 0;
    }
    return 1;
}

auto test_rejects_malformed_dynamic_linkage_metadata() -> int {
    auto duplicate_soname = make_linkage_image(false, false, false, true);
    Dynamic soname{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::SoName), .d_val = 24};
    put_dynamic(duplicate_soname, DYNAMIC_OFFSET + 8 * sizeof(Dynamic), soname, false);
    auto const duplicate_parsed = File::from_raw(duplicate_soname.data(), duplicate_soname.size());
    ustl::Option<DynamicImage> duplicate_image;
    if (duplicate_parsed.is_ok()) {
        duplicate_image = duplicate_parsed->dynamic_image();
    } else {
        duplicate_image = ustl::NONE;
    }
    if (duplicate_image.is_none()) {
        return 1;
    }
    if (duplicate_image->linkage().is_some()) {
        return 2;
    }

    auto invalid_needed = make_linkage_image(false, false, false, true);
    Dynamic needed{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Needed), .d_val = 59};
    put_dynamic(invalid_needed, DYNAMIC_OFFSET + 5 * sizeof(Dynamic), needed, false);
    auto const invalid_parsed = File::from_raw(invalid_needed.data(), invalid_needed.size());
    ustl::Option<DynamicImage> invalid_image;
    if (invalid_parsed.is_ok()) {
        invalid_image = invalid_parsed->dynamic_image();
    } else {
        invalid_image = ustl::NONE;
    }
    if (invalid_image.is_some() && invalid_image->linkage().is_none()) {
        return 0;
    }
    return 2;
}

auto test_observes_flag_only_dynamic_metadata() -> int {
    auto bytes = make_linkage_image(false, false, false, true);
    std::memset(bytes.data() + DYNAMIC_OFFSET, 0, DYNAMIC_COUNT * sizeof(Dynamic));
    Dynamic flags{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Flags), .d_val = 0x11};
    put_dynamic(bytes, DYNAMIC_OFFSET, flags, false);
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    ustl::Option<DynamicImage> image;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    } else {
        image = ustl::NONE;
    }
    ustl::Option<DynamicLinkage> linkage;
    if (image.is_some()) {
        linkage = image->linkage();
    } else {
        linkage = ustl::NONE;
    }
    if (linkage.is_some() && linkage->needed_library_count() == 0 && linkage->flags().is_some() &&
        linkage->flags().value() == 0x11 && linkage->soname().is_none()) {
        return 0;
    }
    return 1;
}

auto test_observes_dynamic_function_arrays(bool big_endian) -> int {
    auto const bytes = make_function_array_image(big_endian);
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    ustl::Option<DynamicImage> image;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    } else {
        image = ustl::NONE;
    }
    ustl::Option<FunctionArray> init;
    if (image.is_some()) {
        init = image->dynamic_init_array();
    } else {
        init = ustl::NONE;
    }
    ustl::Option<FunctionArray> fini;
    if (image.is_some()) {
        fini = image->dynamic_fini_array();
    } else {
        fini = ustl::NONE;
    }
    ustl::Option<FunctionArray> preinit;
    if (image.is_some()) {
        preinit = image->dynamic_preinit_array();
    } else {
        preinit = ustl::NONE;
    }
    ustl::Option<Traits::Addr> init_value;
    if (init.is_some()) {
        init_value = init->at(1);
    } else {
        init_value = ustl::NONE;
    }
    ustl::Option<Traits::Addr> fini_value;
    if (fini.is_some()) {
        fini_value = fini->at(0);
    } else {
        fini_value = ustl::NONE;
    }
    ustl::Option<Traits::Addr> preinit_value;
    if (preinit.is_some()) {
        preinit_value = preinit->at(0);
    } else {
        preinit_value = ustl::NONE;
    }
    ustl::Option<u64> legacy_init;
    if (image.is_some()) {
        legacy_init = image->dynamic_init_function();
    } else {
        legacy_init = ustl::NONE;
    }
    ustl::Option<u64> legacy_fini;
    if (image.is_some()) {
        legacy_fini = image->dynamic_fini_function();
    } else {
        legacy_fini = ustl::NONE;
    }
    if (init.is_some() && init->size() == 3 && init_value.is_some() &&
        init_value.value() == 0x401101 && fini.is_some() && fini->size() == 2 &&
        fini_value.is_some() && fini_value.value() == 0x402001 && preinit.is_some() &&
        preinit->size() == 1 && preinit_value.is_some() && preinit_value.value() == 0x403001 &&
        legacy_init.is_some() && legacy_init.value() == 0x400281 && legacy_fini.is_some() &&
        legacy_fini.value() == 0x400299) {
        return 0;
    }
    return 1;
}

auto test_rejects_malformed_dynamic_function_arrays() -> int {
    auto duplicate = make_function_array_image();
    Dynamic duplicate_init{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::InitArray),
                           .d_val = INIT_ARRAY_ADDRESS};
    put_dynamic(duplicate, DYNAMIC_OFFSET + 7 * sizeof(Dynamic), duplicate_init, false);
    auto const duplicate_parsed = File::from_raw(duplicate.data(), duplicate.size());
    ustl::Option<DynamicImage> duplicate_image;
    if (duplicate_parsed.is_ok()) {
        duplicate_image = duplicate_parsed->dynamic_image();
    } else {
        duplicate_image = ustl::NONE;
    }
    if (duplicate_image.is_none() || duplicate_image->dynamic_init_array().is_some()) {
        return 1;
    }

    auto invalid_size = make_function_array_image();
    Dynamic malformed_size{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::FiniArraySz),
                           .d_val = sizeof(Traits::Addr) - 1};
    put_dynamic(invalid_size, DYNAMIC_OFFSET + 8 * sizeof(Dynamic), malformed_size, false);
    auto const invalid_size_parsed = File::from_raw(invalid_size.data(), invalid_size.size());
    ustl::Option<DynamicImage> invalid_size_image;
    if (invalid_size_parsed.is_ok()) {
        invalid_size_image = invalid_size_parsed->dynamic_image();
    } else {
        invalid_size_image = ustl::NONE;
    }
    if (invalid_size_image.is_none() || invalid_size_image->dynamic_fini_array().is_some()) {
        return 2;
    }

    auto missing_pair = make_function_array_image();
    Dynamic replacement{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Flags), .d_val = 1};
    put_dynamic(missing_pair, DYNAMIC_OFFSET + 9 * sizeof(Dynamic), replacement, false);
    auto const missing_pair_parsed = File::from_raw(missing_pair.data(), missing_pair.size());
    ustl::Option<DynamicImage> missing_pair_image;
    if (missing_pair_parsed.is_ok()) {
        missing_pair_image = missing_pair_parsed->dynamic_image();
    } else {
        missing_pair_image = ustl::NONE;
    }
    if (missing_pair_image.is_none() || missing_pair_image->dynamic_preinit_array().is_some()) {
        return 3;
    }

    auto out_of_file = make_function_array_image();
    Dynamic address{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::InitArray),
                    .d_val = LOAD_ADDRESS + 0x2f8};
    put_dynamic(out_of_file, DYNAMIC_OFFSET + 5 * sizeof(Dynamic), address, false);
    auto const out_of_file_parsed = File::from_raw(out_of_file.data(), out_of_file.size());
    ustl::Option<DynamicImage> out_of_file_image;
    if (out_of_file_parsed.is_ok()) {
        out_of_file_image = out_of_file_parsed->dynamic_image();
    } else {
        out_of_file_image = ustl::NONE;
    }
    if (out_of_file_image.is_some() && out_of_file_image->dynamic_init_array().is_none()) {
        return 0;
    }
    return 4;
}

auto test_discovers_dynamic_version_metadata() -> int {
    auto const bytes = make_linkage_image(false, false, true);
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    ustl::Option<DynamicImage> image;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    } else {
        image = ustl::NONE;
    }
    ustl::Option<VersionSymbols> versions;
    if (image.is_some()) {
        versions = image->dynamic_version_symbols();
    } else {
        versions = ustl::NONE;
    }
    ustl::Option<VersionDefinitions> definitions;
    if (image.is_some()) {
        definitions = image->dynamic_version_definitions();
    } else {
        definitions = ustl::NONE;
    }
    ustl::Option<VersionRequirements> requirements;
    if (image.is_some()) {
        requirements = image->dynamic_version_requirements();
    } else {
        requirements = ustl::NONE;
    }
    ustl::Option<Version> selected_version;
    if (versions.is_some()) {
        selected_version = versions->version_at(2);
    } else {
        selected_version = ustl::NONE;
    }
    ustl::Option<ByteRange> definition_name;
    if (definitions.is_some()) {
        definition_name = definitions->definition_name(0);
    } else {
        definition_name = ustl::NONE;
    }
    ustl::Option<ByteRange> provider;
    if (requirements.is_some()) {
        provider = requirements->file_name(0);
    } else {
        provider = ustl::NONE;
    }
    ustl::Option<ByteRange> requirement_name;
    if (requirements.is_some()) {
        requirement_name = requirements->auxiliary_name(0, 0);
    } else {
        requirement_name = ustl::NONE;
    }
    ustl::Option<VersionBinding> definition_binding;
    if (image.is_some()) {
        definition_binding = image->dynamic_version_binding(1);
    } else {
        definition_binding = ustl::NONE;
    }
    ustl::Option<VersionBinding> requirement_binding;
    if (image.is_some()) {
        requirement_binding = image->dynamic_version_binding(2);
    } else {
        requirement_binding = ustl::NONE;
    }
    if (versions.is_some() && versions->size() == 3 && selected_version.is_some() &&
        eftl::elf::VersionSymbolTableView<Traits>::version_index(selected_version.value()) == 3 &&
        eftl::elf::VersionSymbolTableView<Traits>::is_hidden(selected_version.value()) &&
        definitions.is_some() && definitions->size() == 1 && definition_name.is_some() &&
        equals(definition_name.value(), "VER_DEF", 7) && requirements.is_some() &&
        requirements->size() == 1 && provider.is_some() &&
        equals(provider.value(), "libdep.so", 9) && requirement_name.is_some() &&
        equals(requirement_name.value(), "VER_REQ", 7) && definition_binding.is_some() &&
        definition_binding->m_kind == eftl::elf::VersionBindingKind::Definition &&
        equals(definition_binding->m_name, "VER_DEF", 7) && requirement_binding.is_some() &&
        requirement_binding->m_kind == eftl::elf::VersionBindingKind::Requirement &&
        requirement_binding->m_hidden && equals(requirement_binding->m_provider, "libdep.so", 9)) {
        return 0;
    }
    return 1;
}

auto test_discovers_big_endian_dynamic_version_metadata() -> int {
    auto const bytes = make_linkage_image(true, false, true);
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    ustl::Option<DynamicImage> image;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    } else {
        image = ustl::NONE;
    }
    ustl::Option<VersionDefinitions> definitions;
    if (image.is_some()) {
        definitions = image->dynamic_version_definitions();
    } else {
        definitions = ustl::NONE;
    }
    ustl::Option<VersionRequirements> requirements;
    if (image.is_some()) {
        requirements = image->dynamic_version_requirements();
    } else {
        requirements = ustl::NONE;
    }
    ustl::Option<ByteRange> definition_name;
    if (definitions.is_some()) {
        definition_name = definitions->definition_name(0);
    } else {
        definition_name = ustl::NONE;
    }
    ustl::Option<ByteRange> requirement_name;
    if (requirements.is_some()) {
        requirement_name = requirements->auxiliary_name(0, 0);
    } else {
        requirement_name = ustl::NONE;
    }
    if (definition_name.is_some() && equals(definition_name.value(), "VER_DEF", 7) &&
        requirement_name.is_some() && equals(requirement_name.value(), "VER_REQ", 7)) {
        return 0;
    }
    return 1;
}

auto test_rejects_malformed_dynamic_version_metadata() -> int {
    auto invalid_count = make_linkage_image(false, false, true);
    constexpr u64 INVALID_COUNT = 2;
    put_scalar(invalid_count, DYNAMIC_OFFSET + 7 * sizeof(Dynamic) + offsetof(Dynamic, d_val),
               INVALID_COUNT, false);
    auto const invalid_count_parsed = File::from_raw(invalid_count.data(), invalid_count.size());
    ustl::Option<DynamicImage> invalid_count_image;
    if (invalid_count_parsed.is_ok()) {
        invalid_count_image = invalid_count_parsed->dynamic_image();
    } else {
        invalid_count_image = ustl::NONE;
    }
    if (invalid_count_image.is_none() ||
        invalid_count_image->dynamic_version_definitions().is_some()) {
        return 1;
    }

    auto missing_pair = make_linkage_image(false, false, true);
    Dynamic null{};
    put_dynamic(missing_pair, DYNAMIC_OFFSET + 9 * sizeof(Dynamic), null, false);
    auto const missing_pair_parsed = File::from_raw(missing_pair.data(), missing_pair.size());
    ustl::Option<DynamicImage> missing_pair_image;
    if (missing_pair_parsed.is_ok()) {
        missing_pair_image = missing_pair_parsed->dynamic_image();
    } else {
        missing_pair_image = ustl::NONE;
    }
    if (missing_pair_image.is_none() ||
        missing_pair_image->dynamic_version_requirements().is_some()) {
        return 2;
    }

    auto ambiguous_binding = make_linkage_image(false, false, true);
    RequirementAuxiliary ambiguous{};
    std::memcpy(&ambiguous, ambiguous_binding.data() + VERNEED_OFFSET + sizeof(Requirement),
                sizeof(ambiguous));
    ambiguous.vna_other = 2;
    put_requirement_auxiliary(ambiguous_binding, VERNEED_OFFSET + sizeof(Requirement), ambiguous,
                              false);
    auto const ambiguous_parsed =
        File::from_raw(ambiguous_binding.data(), ambiguous_binding.size());
    ustl::Option<DynamicImage> ambiguous_image;
    if (ambiguous_parsed.is_ok()) {
        ambiguous_image = ambiguous_parsed->dynamic_image();
    } else {
        ambiguous_image = ustl::NONE;
    }
    if (ambiguous_image.is_some() && ambiguous_image->dynamic_version_binding(1).is_none()) {
        return 0;
    }
    return 3;
}

auto test_rejects_malformed_dynamic_linkage_tags() -> int {
    auto duplicate_strings = make_linkage_image();
    Dynamic duplicate{};
    std::memcpy(&duplicate, duplicate_strings.data() + DYNAMIC_OFFSET + sizeof(Dynamic),
                sizeof(duplicate));
    duplicate.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::StrTab);
    put(duplicate_strings, DYNAMIC_OFFSET + sizeof(Dynamic), duplicate);
    auto const duplicate_parsed =
        File::from_raw(duplicate_strings.data(), duplicate_strings.size());
    ustl::Option<DynamicImage> duplicate_image;
    if (duplicate_parsed.is_ok()) {
        duplicate_image = duplicate_parsed->dynamic_image();
    } else {
        duplicate_image = ustl::NONE;
    }
    if (duplicate_image.is_none() || duplicate_image->dynamic_strings().is_some()) {
        return 1;
    }

    auto invalid_entry_size = make_linkage_image();
    Dynamic entry_size{};
    std::memcpy(&entry_size, invalid_entry_size.data() + DYNAMIC_OFFSET + 3 * sizeof(Dynamic),
                sizeof(entry_size));
    entry_size.d_val = sizeof(Symbol) - 1;
    put(invalid_entry_size, DYNAMIC_OFFSET + 3 * sizeof(Dynamic), entry_size);
    auto const entry_size_parsed =
        File::from_raw(invalid_entry_size.data(), invalid_entry_size.size());
    ustl::Option<DynamicImage> entry_size_image;
    if (entry_size_parsed.is_ok()) {
        entry_size_image = entry_size_parsed->dynamic_image();
    } else {
        entry_size_image = ustl::NONE;
    }
    if (entry_size_image.is_none() || entry_size_image->dynamic_symbols().is_some()) {
        return 2;
    }

    auto truncated_hash = make_linkage_image();
    constexpr u32 IMPOSSIBLE_CHAIN_COUNT = 0x100;
    put(truncated_hash, HASH_OFFSET + sizeof(u32), IMPOSSIBLE_CHAIN_COUNT);
    auto const truncated_parsed = File::from_raw(truncated_hash.data(), truncated_hash.size());
    ustl::Option<DynamicImage> truncated_image;
    if (truncated_parsed.is_ok()) {
        truncated_image = truncated_parsed->dynamic_image();
    } else {
        truncated_image = ustl::NONE;
    }
    if (truncated_image.is_none() || truncated_image->sysv_hash_table().is_some()) {
        return 3;
    }

    auto unterminated_gnu_chain = make_linkage_image(false, true);
    constexpr u32 UNTERMINATED_CHAIN = 0x0b8860baU;
    put(unterminated_gnu_chain, HASH_OFFSET + 6 * sizeof(u32) + sizeof(Traits::Addr),
        UNTERMINATED_CHAIN);
    auto const unterminated_parsed =
        File::from_raw(unterminated_gnu_chain.data(), unterminated_gnu_chain.size());
    ustl::Option<DynamicImage> unterminated_image;
    if (unterminated_parsed.is_ok()) {
        unterminated_image = unterminated_parsed->dynamic_image();
    } else {
        unterminated_image = ustl::NONE;
    }
    if (unterminated_image.is_some() && unterminated_image->gnu_hash_table().is_none()) {
        return 0;
    }
    return 4;
}
} // namespace

auto main() -> int {
    if (auto const result = test_discovers_dynamic_strings_symbols_and_sysv_hash(); result != 0) {
        return result;
    }
    if (auto const result = test_discovers_big_endian_dynamic_linkage(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_discovers_dynamic_gnu_hash(); result != 0) {
        return 20 + result;
    }
    if (auto const result = test_discovers_big_endian_dynamic_gnu_hash(); result != 0) {
        return 30 + result;
    }
    if (auto const result = test_observes_ordered_dynamic_linkage_metadata(false); result != 0) {
        return 40 + result;
    }
    if (auto const result = test_observes_ordered_dynamic_linkage_metadata(true); result != 0) {
        return 50 + result;
    }
    if (auto const result = test_rejects_malformed_dynamic_linkage_metadata(); result != 0) {
        return 60 + result;
    }
    if (auto const result = test_observes_flag_only_dynamic_metadata(); result != 0) {
        return 70 + result;
    }
    if (auto const result = test_observes_dynamic_function_arrays(false); result != 0) {
        return 75 + result;
    }
    if (auto const result = test_observes_dynamic_function_arrays(true); result != 0) {
        return 80 + result;
    }
    if (auto const result = test_rejects_malformed_dynamic_function_arrays(); result != 0) {
        return 85 + result;
    }
    if (auto const result = test_discovers_dynamic_version_metadata(); result != 0) {
        return 90 + result;
    }
    if (auto const result = test_discovers_big_endian_dynamic_version_metadata(); result != 0) {
        return 100 + result;
    }
    if (auto const result = test_rejects_malformed_dynamic_version_metadata(); result != 0) {
        return 110 + result;
    }
    if (auto const result = test_rejects_malformed_dynamic_linkage_tags(); result == 0) {
        return 0;
    }
    return 120;
}
