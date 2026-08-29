/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/dynamic_image.hpp>

#include <array>
#include <type_traits>

#include <cstddef>
#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using File = eftl::elf::FileImpl<Traits>;
using Dynamic = Traits::Dyn;
using Header = Traits::Header;
using Rel = Traits::Rel;
using Rela = Traits::Rela;
using Segment = Traits::Segment;
using Symbol = Traits::Symbol;
using DynamicImage = eftl::elf::DynamicImageView<Traits>;
using Relocations = eftl::elf::RelocationTableView<Traits, Rel>;
using RelaRelocations = eftl::elf::RelocationTableView<Traits, Rela>;
using Relr = eftl::elf::TableView<Traits::Addr>;

constexpr usize PROGRAM_HEADERS_OFFSET = sizeof(Header);
constexpr usize LOAD_OFFSET = 0x300;
constexpr u64 LOAD_ADDRESS = 0x400000;
constexpr usize DYNAMIC_OFFSET = LOAD_OFFSET + 0x40;
constexpr u64 DYNAMIC_ADDRESS = LOAD_ADDRESS + 0x40;
constexpr usize DYNAMIC_COUNT = 18;
constexpr usize STRING_OFFSET = LOAD_OFFSET + 0x180;
constexpr u64 STRING_ADDRESS = LOAD_ADDRESS + 0x180;
constexpr usize SYMBOL_OFFSET = LOAD_OFFSET + 0x1a0;
constexpr u64 SYMBOL_ADDRESS = LOAD_ADDRESS + 0x1a0;
constexpr usize HASH_OFFSET = LOAD_OFFSET + 0x200;
constexpr u64 HASH_ADDRESS = LOAD_ADDRESS + 0x200;
constexpr usize REL_OFFSET = LOAD_OFFSET + 0x240;
constexpr u64 REL_ADDRESS = LOAD_ADDRESS + 0x240;
constexpr usize RELA_OFFSET = LOAD_OFFSET + 0x260;
constexpr u64 RELA_ADDRESS = LOAD_ADDRESS + 0x260;
constexpr usize RELR_OFFSET = LOAD_OFFSET + 0x280;
constexpr u64 RELR_ADDRESS = LOAD_ADDRESS + 0x280;
constexpr usize PLT_RELA_OFFSET = LOAD_OFFSET + 0x2a0;
constexpr u64 PLT_RELA_ADDRESS = LOAD_ADDRESS + 0x2a0;

template <typename Value, usize Size>
auto put_scalar(std::array<u8, Size> &image, usize offset, Value value, bool big_endian) -> void {
    static_assert(std::is_unsigned_v<Value>);
    if (!big_endian) {
        std::memcpy(image.data() + offset, &value, sizeof(value));
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
auto put_rel(std::array<u8, Size> &image, usize offset, Rel const &relocation, bool big_endian)
    -> void {
    put_scalar(image, offset + offsetof(Rel, r_offset), relocation.r_offset, big_endian);
    put_scalar(image, offset + offsetof(Rel, r_info), relocation.r_info, big_endian);
}

template <usize Size>
auto put_rela(std::array<u8, Size> &image, usize offset, Rela const &relocation, bool big_endian)
    -> void {
    put_scalar(image, offset + offsetof(Rela, r_offset), relocation.r_offset, big_endian);
    put_scalar(image, offset + offsetof(Rela, r_info), relocation.r_info, big_endian);
    put_scalar(image, offset + offsetof(Rela, r_addend), static_cast<u64>(relocation.r_addend),
               big_endian);
}

auto make_dynamic_relocation_image(bool big_endian = false, bool plt_rel = false)
    -> std::array<u8, 2048> {
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
    load.p_filesz = 0x500;
    load.p_memsz = 0x500;
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

    std::array<Dynamic, DYNAMIC_COUNT> entries{
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::StrTab),
                .d_val = STRING_ADDRESS},
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::StrSz), .d_val = 9},
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::SymTab),
                .d_val = SYMBOL_ADDRESS},
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Syment),
                .d_val = sizeof(Symbol)},
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Hash),
                .d_val = HASH_ADDRESS},
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Rel), .d_val = REL_ADDRESS},
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::RelSz),
                .d_val = sizeof(Rel)},
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::RelEnt),
                .d_val = sizeof(Rel)},
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Rela),
                .d_val = RELA_ADDRESS},
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::RelaSz),
                .d_val = sizeof(Rela)},
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::RelaEnt),
                .d_val = sizeof(Rela)},
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Relr),
                .d_val = RELR_ADDRESS},
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::RelrSz),
                .d_val = 2 * sizeof(Traits::Addr)},
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::RelrEnt),
                .d_val = sizeof(Traits::Addr)},
        Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::JmpRel),
                .d_val = PLT_RELA_ADDRESS},
    };
    if (plt_rel) {
        entries[15] = Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::PltRelSz),
                              .d_val = sizeof(Rel)};
        entries[16] = Dynamic{.d_tag = static_cast<u64>(eftl::elf::DynTag::Rel)};
    } else {
        entries[15] = Dynamic{.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::PltRelSz),
                              .d_val = sizeof(Rela)};
        entries[16] = Dynamic{.d_tag = static_cast<u64>(eftl::elf::DynTag::Rela)};
    }
    for (usize index = 0; index < entries.size(); ++index) {
        put_dynamic(image, DYNAMIC_OFFSET + index * sizeof(Dynamic), entries[index], big_endian);
    }

    std::array<u8, 9> const strings{0, 'f', 'o', 'o', 0, 'b', 'a', 'r', 0};
    std::memcpy(image.data() + STRING_OFFSET, strings.data(), strings.size());
    Symbol foo{};
    foo.st_name = 1;
    put_symbol(image, SYMBOL_OFFSET + sizeof(Symbol), foo, big_endian);
    Symbol bar{};
    bar.st_name = 5;
    put_symbol(image, SYMBOL_OFFSET + 2 * sizeof(Symbol), bar, big_endian);

    std::array<u32, 6> const hash_words{1, 3, 1, 0, 2, 0};
    for (usize index = 0; index < hash_words.size(); ++index) {
        put_scalar(image, HASH_OFFSET + index * sizeof(u32), hash_words[index], big_endian);
    }
    Rel relative{};
    relative.r_offset = 0x400100;
    relative.r_info = 8;
    put_rel(image, REL_OFFSET, relative, big_endian);
    Rela named{};
    named.r_offset = 0x400200;
    named.r_info = (static_cast<u64>(1) << 32U) | 1U;
    named.r_addend = -16;
    put_rela(image, RELA_OFFSET, named, big_endian);
    put_scalar(image, RELR_OFFSET, static_cast<Traits::Addr>(0x400300), big_endian);
    put_scalar(image, RELR_OFFSET + sizeof(Traits::Addr), static_cast<Traits::Addr>(3), big_endian);
    if (plt_rel) {
        Rel plt{};
        plt.r_offset = 0x400400;
        plt.r_info = (static_cast<u64>(2) << 32U) | 1U;
        put_rel(image, PLT_RELA_OFFSET, plt, big_endian);
    } else {
        Rela plt{};
        plt.r_offset = 0x400400;
        plt.r_info = (static_cast<u64>(2) << 32U) | 1U;
        plt.r_addend = 4;
        put_rela(image, PLT_RELA_OFFSET, plt, big_endian);
    }
    return image;
}

auto test_observes_dynamic_relocation_tables() -> int {
    auto const bytes = make_dynamic_relocation_image();
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    ustl::Option<DynamicImage> image;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    } else {
        image = ustl::NONE;
    }
    ustl::Option<Relocations> rel;
    if (image.is_some()) {
        rel = image->dynamic_rel_table();
    } else {
        rel = ustl::NONE;
    }
    ustl::Option<RelaRelocations> rela;
    if (image.is_some()) {
        rela = image->dynamic_rela_table();
    } else {
        rela = ustl::NONE;
    }
    ustl::Option<Relr> relr;
    if (image.is_some()) {
        relr = image->dynamic_relr_table();
    } else {
        relr = ustl::NONE;
    }
    ustl::Option<RelaRelocations> plt_rela;
    if (image.is_some()) {
        plt_rela = image->dynamic_plt_rela_table();
    } else {
        plt_rela = ustl::NONE;
    }
    ustl::Option<Rel> relative;
    if (rel.is_some()) {
        relative = rel->relocation_at(0);
    } else {
        relative = ustl::NONE;
    }
    ustl::Option<Rela> named;
    if (rela.is_some()) {
        named = rela->relocation_at(0);
    } else {
        named = ustl::NONE;
    }
    ustl::Option<Rela> plt;
    if (plt_rela.is_some()) {
        plt = plt_rela->relocation_at(0);
    } else {
        plt = ustl::NONE;
    }
    ustl::Option<Traits::Addr> relr_start;
    if (relr.is_some()) {
        relr_start = relr->at(0);
    } else {
        relr_start = ustl::NONE;
    }
    ustl::Option<Traits::Addr> relr_bitmap;
    if (relr.is_some()) {
        relr_bitmap = relr->at(1);
    } else {
        relr_bitmap = ustl::NONE;
    }
    if (rel.is_some() && rel->size() == 1 && relative.is_some() &&
        eftl::elf::RelocationTableView<Traits, Rel>::type(relative.value()) == 8 &&
        rela.is_some() && named.is_some() && named->r_addend == -16 &&
        eftl::elf::RelocationTableView<Traits, Rela>::symbol_index(named.value()) == 1 &&
        relr.is_some() && relr->size() == 2 && relr_start.is_some() &&
        relr_start.value() == 0x400300 && relr_bitmap.is_some() && relr_bitmap.value() == 3 &&
        plt_rela.is_some() && plt.is_some() && plt->r_offset == 0x400400 &&
        image->dynamic_plt_rel_table().is_none()) {
        return 0;
    }
    return 1;
}

auto test_observes_big_endian_dynamic_relocations() -> int {
    auto const bytes = make_dynamic_relocation_image(true);
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    ustl::Option<DynamicImage> image;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    } else {
        image = ustl::NONE;
    }
    ustl::Option<RelaRelocations> rela;
    if (image.is_some()) {
        rela = image->dynamic_rela_table();
    } else {
        rela = ustl::NONE;
    }
    ustl::Option<Relr> relr;
    if (image.is_some()) {
        relr = image->dynamic_relr_table();
    } else {
        relr = ustl::NONE;
    }
    ustl::Option<Rela> named;
    if (rela.is_some()) {
        named = rela->relocation_at(0);
    } else {
        named = ustl::NONE;
    }
    ustl::Option<Traits::Addr> bitmap;
    if (relr.is_some()) {
        bitmap = relr->at(1);
    } else {
        bitmap = ustl::NONE;
    }
    if (named.is_some() && named->r_offset == 0x400200 && named->r_addend == -16 &&
        bitmap.is_some() && bitmap.value() == 3) {
        return 0;
    }
    return 1;
}

auto test_observes_rel_encoded_plt_relocations() -> int {
    auto const bytes = make_dynamic_relocation_image(false, true);
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    ustl::Option<DynamicImage> image;
    if (parsed.is_ok()) {
        image = parsed->dynamic_image();
    } else {
        image = ustl::NONE;
    }
    ustl::Option<Relocations> plt;
    if (image.is_some()) {
        plt = image->dynamic_plt_rel_table();
    } else {
        plt = ustl::NONE;
    }
    ustl::Option<Rel> relocation;
    if (plt.is_some()) {
        relocation = plt->relocation_at(0);
    } else {
        relocation = ustl::NONE;
    }
    if (plt.is_some() && relocation.is_some() && relocation->r_offset == 0x400400 &&
        eftl::elf::RelocationTableView<Traits, Rel>::symbol_index(relocation.value()) == 2 &&
        image->dynamic_plt_rela_table().is_none()) {
        return 0;
    }
    return 1;
}

auto test_rejects_malformed_dynamic_relocation_metadata() -> int {
    auto bad_entry_size = make_dynamic_relocation_image();
    constexpr u64 BAD_REL_ENT = sizeof(Rel) - 1;
    put_scalar(bad_entry_size, DYNAMIC_OFFSET + 7 * sizeof(Dynamic) + offsetof(Dynamic, d_val),
               BAD_REL_ENT, false);
    auto const bad_entry_parsed = File::from_raw(bad_entry_size.data(), bad_entry_size.size());
    ustl::Option<DynamicImage> bad_entry_image;
    if (bad_entry_parsed.is_ok()) {
        bad_entry_image = bad_entry_parsed->dynamic_image();
    } else {
        bad_entry_image = ustl::NONE;
    }
    if (bad_entry_image.is_none() || bad_entry_image->dynamic_rel_table().is_some()) {
        return 1;
    }

    auto invalid_plt_type = make_dynamic_relocation_image();
    constexpr u64 BAD_PLT_TYPE = 0;
    put_scalar(invalid_plt_type, DYNAMIC_OFFSET + 16 * sizeof(Dynamic) + offsetof(Dynamic, d_val),
               BAD_PLT_TYPE, false);
    auto const invalid_plt_parsed =
        File::from_raw(invalid_plt_type.data(), invalid_plt_type.size());
    ustl::Option<DynamicImage> invalid_plt_image;
    if (invalid_plt_parsed.is_ok()) {
        invalid_plt_image = invalid_plt_parsed->dynamic_image();
    } else {
        invalid_plt_image = ustl::NONE;
    }
    if (invalid_plt_image.is_none() || invalid_plt_image->dynamic_plt_rel_table().is_some() ||
        invalid_plt_image->dynamic_plt_rela_table().is_some()) {
        return 2;
    }

    auto out_of_range_symbol = make_dynamic_relocation_image();
    Rela bad_symbol{};
    std::memcpy(&bad_symbol, out_of_range_symbol.data() + RELA_OFFSET, sizeof(bad_symbol));
    bad_symbol.r_info = (static_cast<u64>(3) << 32U) | 1U;
    put_rela(out_of_range_symbol, RELA_OFFSET, bad_symbol, false);
    auto const bad_symbol_parsed =
        File::from_raw(out_of_range_symbol.data(), out_of_range_symbol.size());
    ustl::Option<DynamicImage> bad_symbol_image;
    if (bad_symbol_parsed.is_ok()) {
        bad_symbol_image = bad_symbol_parsed->dynamic_image();
    } else {
        bad_symbol_image = ustl::NONE;
    }
    if (bad_symbol_image.is_none() || bad_symbol_image->dynamic_rela_table().is_some()) {
        return 3;
    }

    auto duplicate_rel = make_dynamic_relocation_image();
    Dynamic duplicate{};
    std::memcpy(&duplicate, duplicate_rel.data() + DYNAMIC_OFFSET + 6 * sizeof(Dynamic),
                sizeof(duplicate));
    duplicate.d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Rel);
    put_dynamic(duplicate_rel, DYNAMIC_OFFSET + 6 * sizeof(Dynamic), duplicate, false);
    auto const duplicate_parsed = File::from_raw(duplicate_rel.data(), duplicate_rel.size());
    ustl::Option<DynamicImage> duplicate_image;
    if (duplicate_parsed.is_ok()) {
        duplicate_image = duplicate_parsed->dynamic_image();
    } else {
        duplicate_image = ustl::NONE;
    }
    if (duplicate_image.is_some() && duplicate_image->dynamic_rel_table().is_none()) {
        return 0;
    }
    return 4;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_dynamic_relocation_tables(); result != 0) {
        return result;
    }
    if (auto const result = test_observes_big_endian_dynamic_relocations(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_observes_rel_encoded_plt_relocations(); result != 0) {
        return 20 + result;
    }
    if (auto const result = test_rejects_malformed_dynamic_relocation_metadata(); result == 0) {
        return 0;
    }
    return 30;
}
