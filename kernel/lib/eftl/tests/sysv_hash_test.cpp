/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/sysv_hash.hpp>

#include <array>
#include <type_traits>

#include <cstddef>
#include <cstring>

namespace {
using Traits = eftl::elf::Traits64;
using Binary = eftl::elf::FileImpl<Traits>;
using Header = Traits::Header;
using Section = Traits::Section;
using Symbol = Traits::Symbol;

constexpr usize K_SECTION_TABLE_OFFSET = sizeof(Header);
constexpr usize K_STRING_TABLE_OFFSET = 0x200;
constexpr usize K_DYNSYM_OFFSET = 0x240;
constexpr usize K_HASH_OFFSET = 0x2a0;

template <typename T, usize Size>
auto put(std::array<u8, Size> &image, usize offset, T const &value) -> void {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

template <typename Value, usize Size>
auto put_big(std::array<u8, Size> &image, usize offset, Value value) -> void {
    static_assert(std::is_unsigned_v<Value>);
    for (usize index = 0; index < sizeof(Value); ++index) {
        auto const shift = (sizeof(Value) - index - 1) * 8U;
        image[offset + index] = static_cast<u8>(value >> shift);
    }
}

template <usize Size>
auto put_big_header(std::array<u8, Size> &image, Header const &header) -> void {
    std::memcpy(image.data(), header.e_ident, sizeof(header.e_ident));
    put_big(image, offsetof(Header, e_type), header.e_type);
    put_big(image, offsetof(Header, e_machine), header.e_machine);
    put_big(image, offsetof(Header, e_version), header.e_version);
    put_big(image, offsetof(Header, e_entry), header.e_entry);
    put_big(image, offsetof(Header, e_phoff), header.e_phoff);
    put_big(image, offsetof(Header, e_shoff), header.e_shoff);
    put_big(image, offsetof(Header, e_flags), header.e_flags);
    put_big(image, offsetof(Header, e_ehsize), header.e_ehsize);
    put_big(image, offsetof(Header, e_phentsize), header.e_phentsize);
    put_big(image, offsetof(Header, e_phnum), header.e_phnum);
    put_big(image, offsetof(Header, e_shentsize), header.e_shentsize);
    put_big(image, offsetof(Header, e_shnum), header.e_shnum);
    put_big(image, offsetof(Header, e_shstrndx), header.e_shstrndx);
}

template <usize Size>
auto put_big_section(std::array<u8, Size> &image, usize offset, Section const &section)
    -> void {
    put_big(image, offset + offsetof(Section, sh_name), section.sh_name);
    put_big(image, offset + offsetof(Section, sh_type), section.sh_type);
    put_big(image, offset + offsetof(Section, sh_flags), section.sh_flags);
    put_big(image, offset + offsetof(Section, sh_addr), section.sh_addr);
    put_big(image, offset + offsetof(Section, sh_offset), section.sh_offset);
    put_big(image, offset + offsetof(Section, sh_size), section.sh_size);
    put_big(image, offset + offsetof(Section, sh_link), section.sh_link);
    put_big(image, offset + offsetof(Section, sh_info), section.sh_info);
    put_big(image, offset + offsetof(Section, sh_addralign), section.sh_addralign);
    put_big(image, offset + offsetof(Section, sh_entsize), section.sh_entsize);
}

template <usize Size>
auto put_big_symbol(std::array<u8, Size> &image, usize offset, Symbol const &symbol)
    -> void {
    put_big(image, offset + offsetof(Symbol, st_name), symbol.st_name);
    image[offset + offsetof(Symbol, st_info)] = symbol.st_info;
    image[offset + offsetof(Symbol, st_other)] = symbol.st_other;
    put_big(image, offset + offsetof(Symbol, st_shndx), symbol.st_shndx);
    put_big(image, offset + offsetof(Symbol, st_value), symbol.st_value);
    put_big(image, offset + offsetof(Symbol, st_size), symbol.st_size);
}

auto make_hash_image() -> std::array<u8, 1024> {
    std::array<u8, 1024> image{};
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    header.e_ident[4] = Traits::K_ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = 3;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    header.e_shoff = K_SECTION_TABLE_OFFSET;
    header.e_shentsize = sizeof(Section);
    header.e_shnum = 4;
    put(image, 0, header);

    Section strings{};
    strings.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = K_STRING_TABLE_OFFSET;
    strings.sh_size = 9;
    strings.sh_addralign = 1;

    Section dynamic_symbols{};
    dynamic_symbols.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Dynsym);
    dynamic_symbols.sh_offset = K_DYNSYM_OFFSET;
    dynamic_symbols.sh_size = 3 * sizeof(Symbol);
    dynamic_symbols.sh_link = 1;
    dynamic_symbols.sh_info = 1;
    dynamic_symbols.sh_addralign = alignof(Symbol);
    dynamic_symbols.sh_entsize = sizeof(Symbol);

    Section hash{};
    hash.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Hash);
    hash.sh_offset = K_HASH_OFFSET;
    hash.sh_size = 6 * sizeof(ours::u32);
    hash.sh_link = 2;
    hash.sh_addralign = alignof(ours::u32);

    put(image, K_SECTION_TABLE_OFFSET + sizeof(Section), strings);
    put(image, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), dynamic_symbols);
    put(image, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), hash);

    std::array<u8, 9> const names{0, 'f', 'o', 'o', 0, 'b', 'a', 'r', 0};
    std::memcpy(image.data() + K_STRING_TABLE_OFFSET, names.data(), names.size());
    Symbol foo{};
    foo.st_name = 1;
    put(image, K_DYNSYM_OFFSET + sizeof(Symbol), foo);
    Symbol bar{};
    bar.st_name = 5;
    put(image, K_DYNSYM_OFFSET + 2 * sizeof(Symbol), bar);

    std::array<ours::u32, 6> const words{1, 3, 1, 0, 2, 0};
    std::memcpy(image.data() + K_HASH_OFFSET, words.data(), sizeof(words));
    return image;
}

auto make_big_endian_hash_image() -> std::array<u8, 1024> {
    std::array<u8, 1024> image{};
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    header.e_ident[4] = Traits::K_ELF_CLASS;
    header.e_ident[5] = 2;
    header.e_ident[6] = 1;
    header.e_type = 3;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    header.e_shoff = K_SECTION_TABLE_OFFSET;
    header.e_shentsize = sizeof(Section);
    header.e_shnum = 4;
    put_big_header(image, header);

    Section strings{};
    strings.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = K_STRING_TABLE_OFFSET;
    strings.sh_size = 9;
    strings.sh_addralign = 1;
    Section dynamic_symbols{};
    dynamic_symbols.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Dynsym);
    dynamic_symbols.sh_offset = K_DYNSYM_OFFSET;
    dynamic_symbols.sh_size = 3 * sizeof(Symbol);
    dynamic_symbols.sh_link = 1;
    dynamic_symbols.sh_info = 1;
    dynamic_symbols.sh_addralign = alignof(Symbol);
    dynamic_symbols.sh_entsize = sizeof(Symbol);
    Section hash{};
    hash.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Hash);
    hash.sh_offset = K_HASH_OFFSET;
    hash.sh_size = 6 * sizeof(ours::u32);
    hash.sh_link = 2;
    hash.sh_addralign = alignof(ours::u32);
    put_big_section(image, K_SECTION_TABLE_OFFSET + sizeof(Section), strings);
    put_big_section(image, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), dynamic_symbols);
    put_big_section(image, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), hash);

    std::array<u8, 9> const names{0, 'f', 'o', 'o', 0, 'b', 'a', 'r', 0};
    std::memcpy(image.data() + K_STRING_TABLE_OFFSET, names.data(), names.size());
    Symbol foo{};
    foo.st_name = 1;
    put_big_symbol(image, K_DYNSYM_OFFSET + sizeof(Symbol), foo);
    Symbol bar{};
    bar.st_name = 5;
    put_big_symbol(image, K_DYNSYM_OFFSET + 2 * sizeof(Symbol), bar);
    std::array<ours::u32, 6> const words{1, 3, 1, 0, 2, 0};
    for (usize index = 0; index < words.size(); ++index) {
        put_big(image, K_HASH_OFFSET + index * sizeof(ours::u32), words[index]);
    }
    return image;
}

auto test_observes_and_looks_up_hashes() -> int {
    auto const image = make_hash_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    ustl::Option<eftl::elf::SysvHashTableView<Traits>> hash = ustl::NONE;
    if (parsed.is_ok()) {
        hash = parsed->sysv_hash_table(3);
    }
    std::array<u8, 3> const foo{'f', 'o', 'o'};
    std::array<u8, 3> const bar{'b', 'a', 'r'};
    std::array<u8, 3> const baz{'b', 'a', 'z'};
    ustl::Option<usize> foo_index = ustl::NONE;
    if (hash.is_some()) {
        foo_index = hash->lookup(eftl::ByteRange(foo.data(), foo.size()));
    }
    ustl::Option<usize> bar_index = ustl::NONE;
    if (hash.is_some()) {
        bar_index = hash->lookup(eftl::ByteRange(bar.data(), bar.size()));
    }
    ustl::Option<usize> baz_index = ustl::NONE;
    if (hash.is_some()) {
        baz_index = hash->lookup(eftl::ByteRange(baz.data(), baz.size()));
    }
    auto const foo_hash =
        eftl::elf::SysvHashTableView<Traits>::hash(eftl::ByteRange(foo.data(), foo.size()));
    if (hash.is_some() && hash->bucket_count() == 1 && hash->chain_count() == 3 &&
        hash->bucket_at(0).is_some() && hash->bucket_at(0).value() == 1 &&
        hash->chain_at(1).is_some() && hash->chain_at(1).value() == 2 && foo_index.is_some() &&
        foo_index.value() == 1 && bar_index.is_some() && bar_index.value() == 2 &&
        baz_index.is_none() && hash->bucket_at(1).is_none() &&
        parsed->sysv_hash_table(2).is_none() && foo_hash.is_some() && foo_hash.value() == 27999U &&
        eftl::elf::SysvHashTableView<Traits>::hash(eftl::ByteRange(nullptr, 1)).is_none() &&
        hash->lookup(eftl::ByteRange(nullptr, 1)).is_none()) {
        return 0;
    }
    return 1;
}

auto test_observes_big_endian_hashes() -> int {
    auto const image = make_big_endian_hash_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    ustl::Option<eftl::elf::SysvHashTableView<Traits>> hash = ustl::NONE;
    if (parsed.is_ok()) {
        hash = parsed->sysv_hash_table(3);
    }
    std::array<u8, 3> const foo{'f', 'o', 'o'};
    std::array<u8, 3> const bar{'b', 'a', 'r'};
    ustl::Option<usize> foo_index = ustl::NONE;
    if (hash.is_some()) {
        foo_index = hash->lookup(eftl::ByteRange(foo.data(), foo.size()));
    }
    ustl::Option<usize> bar_index = ustl::NONE;
    if (hash.is_some()) {
        bar_index = hash->lookup(eftl::ByteRange(bar.data(), bar.size()));
    }
    if (hash.is_some() && hash->bucket_at(0).is_some() && hash->bucket_at(0).value() == 1 &&
        hash->chain_at(1).is_some() && hash->chain_at(1).value() == 2 && foo_index.is_some() &&
        foo_index.value() == 1 && bar_index.is_some() && bar_index.value() == 2) {
        return 0;
    }
    return 1;
}

auto test_rejects_invalid_hash_tables() -> int {
    auto wrong_link = make_hash_image();
    Section hash{};
    std::memcpy(&hash, wrong_link.data() + K_SECTION_TABLE_OFFSET + 3 * sizeof(Section),
                sizeof(hash));
    hash.sh_link = 1;
    put(wrong_link, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), hash);
    auto const wrong_link_result = Binary::from_raw(wrong_link.data(), wrong_link.size());
    if (wrong_link_result.is_ok() ||
        wrong_link_result.unwrap_err() != eftl::elf::ParseError::InvalidHash) {
        return 1;
    }

    auto mismatched_count = make_hash_image();
    const ours::u32 wrong_nchain = 2;
    put(mismatched_count, K_HASH_OFFSET + sizeof(ours::u32), wrong_nchain);
    auto const mismatched_count_result =
        Binary::from_raw(mismatched_count.data(), mismatched_count.size());
    if (mismatched_count_result.is_ok() ||
        mismatched_count_result.unwrap_err() != eftl::elf::ParseError::InvalidHash) {
        return 2;
    }

    auto out_of_range_bucket = make_hash_image();
    const ours::u32 invalid_index = 3;
    put(out_of_range_bucket, K_HASH_OFFSET + 2 * sizeof(ours::u32), invalid_index);
    auto const out_of_range_bucket_result =
        Binary::from_raw(out_of_range_bucket.data(), out_of_range_bucket.size());
    if (out_of_range_bucket_result.is_ok() ||
        out_of_range_bucket_result.unwrap_err() != eftl::elf::ParseError::InvalidHash) {
        return 3;
    }

    auto cyclic_chain = make_hash_image();
    const ours::u32 self = 1;
    put(cyclic_chain, K_HASH_OFFSET + 4 * sizeof(ours::u32), self);
    auto const cyclic_chain_result = Binary::from_raw(cyclic_chain.data(), cyclic_chain.size());
    if (cyclic_chain_result.is_ok() ||
        cyclic_chain_result.unwrap_err() != eftl::elf::ParseError::InvalidHash) {
        return 4;
    }

    auto overlapping_bucket_chains = make_hash_image();
    std::memcpy(&hash,
                overlapping_bucket_chains.data() + K_SECTION_TABLE_OFFSET + 3 * sizeof(Section),
                sizeof(hash));
    hash.sh_size = 7 * sizeof(ours::u32);
    put(overlapping_bucket_chains, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), hash);
    std::array<ours::u32, 7> const overlapping_words{2, 3, 1, 1, 0, 2, 0};
    std::memcpy(overlapping_bucket_chains.data() + K_HASH_OFFSET, overlapping_words.data(),
                sizeof(overlapping_words));
    auto const overlapping_bucket_chains_result =
        Binary::from_raw(overlapping_bucket_chains.data(), overlapping_bucket_chains.size());
    if (overlapping_bucket_chains_result.is_ok() ||
        overlapping_bucket_chains_result.unwrap_err() != eftl::elf::ParseError::InvalidHash) {
        return 5;
    }

    auto misbucketed_symbols = make_hash_image();
    std::memcpy(&hash, misbucketed_symbols.data() + K_SECTION_TABLE_OFFSET + 3 * sizeof(Section),
                sizeof(hash));
    hash.sh_size = 7 * sizeof(ours::u32);
    put(misbucketed_symbols, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), hash);
    std::array<ours::u32, 7> const misbucketed_words{2, 3, 1, 2, 0, 0, 0};
    std::memcpy(misbucketed_symbols.data() + K_HASH_OFFSET, misbucketed_words.data(),
                sizeof(misbucketed_words));
    auto const misbucketed_symbols_result =
        Binary::from_raw(misbucketed_symbols.data(), misbucketed_symbols.size());
    if (misbucketed_symbols_result.is_ok() ||
        misbucketed_symbols_result.unwrap_err() != eftl::elf::ParseError::InvalidHash) {
        return 6;
    }

    auto truncated_table = make_hash_image();
    std::memcpy(&hash, truncated_table.data() + K_SECTION_TABLE_OFFSET + 3 * sizeof(Section),
                sizeof(hash));
    hash.sh_size -= 1;
    put(truncated_table, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), hash);
    auto const truncated_table_result =
        Binary::from_raw(truncated_table.data(), truncated_table.size());
    if (truncated_table_result.is_err() &&
        truncated_table_result.unwrap_err() == eftl::elf::ParseError::InvalidHash) {
        return 0;
    }
    return 7;
}

auto test_elf32_observes_sysv_hashes() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Header32 = Traits32::Header;
    using Section32 = Traits32::Section;
    using Symbol32 = Traits32::Symbol;

    constexpr usize K_SECTION_OFFSET = sizeof(Header32);
    constexpr usize K_STRING_OFFSET = 0x100;
    constexpr usize K_DYNSYM_OFFSET_32 = 0x120;
    constexpr usize K_HASH_OFFSET_32 = 0x160;
    std::array<u8, 512> image{};

    Header32 header{};
    std::memcpy(header.e_ident, eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    header.e_ident[4] = Traits32::K_ELF_CLASS;
    header.e_ident[5] = 1;
    header.e_ident[6] = 1;
    header.e_type = 3;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header32);
    header.e_shoff = K_SECTION_OFFSET;
    header.e_shentsize = sizeof(Section32);
    header.e_shnum = 4;
    put(image, 0, header);

    Section32 strings{};
    strings.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Strtab);
    strings.sh_offset = K_STRING_OFFSET;
    strings.sh_size = 5;
    strings.sh_addralign = 1;
    Section32 dynamic_symbols{};
    dynamic_symbols.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Dynsym);
    dynamic_symbols.sh_offset = K_DYNSYM_OFFSET_32;
    dynamic_symbols.sh_size = 2 * sizeof(Symbol32);
    dynamic_symbols.sh_link = 1;
    dynamic_symbols.sh_info = 1;
    dynamic_symbols.sh_addralign = alignof(Symbol32);
    dynamic_symbols.sh_entsize = sizeof(Symbol32);
    Section32 hash{};
    hash.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Hash);
    hash.sh_offset = K_HASH_OFFSET_32;
    hash.sh_size = 5 * sizeof(ours::u32);
    hash.sh_link = 2;
    hash.sh_addralign = alignof(ours::u32);
    put(image, K_SECTION_OFFSET + sizeof(Section32), strings);
    put(image, K_SECTION_OFFSET + 2 * sizeof(Section32), dynamic_symbols);
    put(image, K_SECTION_OFFSET + 3 * sizeof(Section32), hash);

    std::array<u8, 5> const names{0, 'f', 'o', 'o', 0};
    std::memcpy(image.data() + K_STRING_OFFSET, names.data(), names.size());
    Symbol32 foo{};
    foo.st_name = 1;
    put(image, K_DYNSYM_OFFSET_32 + sizeof(Symbol32), foo);
    std::array<ours::u32, 5> const words{1, 2, 1, 0, 0};
    std::memcpy(image.data() + K_HASH_OFFSET_32, words.data(), sizeof(words));

    std::array<u8, 3> const foo_name{'f', 'o', 'o'};
    auto const parsed = Binary32::from_raw(image.data(), image.size());
    ustl::Option<eftl::elf::SysvHashTableView<Traits32>> hash_table = ustl::NONE;
    if (parsed.is_ok()) {
        hash_table = parsed->sysv_hash_table(3);
    }
    ustl::Option<usize> symbol_index = ustl::NONE;
    if (hash_table.is_some()) {
        symbol_index = hash_table->lookup(eftl::ByteRange(foo_name.data(), foo_name.size()));
    }
    if (hash_table.is_some() && hash_table->chain_count() == 2 && symbol_index.is_some() &&
        symbol_index.value() == 1) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_and_looks_up_hashes(); result != 0) {
        return result;
    }
    if (auto const result = test_rejects_invalid_hash_tables(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_elf32_observes_sysv_hashes(); result != 0) {
        return 20 + result;
    }
    if (test_observes_big_endian_hashes() == 0) {
        return 0;
    }
    return 30;
}
