/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>
#include <eftl/elf/gnu_hash.hpp>

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
constexpr usize K_GNU_HASH_OFFSET = 0x2a0;

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

auto make_gnu_hash_image() -> std::array<u8, 1024> {
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
    strings.sh_size = 13;
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
    hash.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::GnuHash);
    hash.sh_offset = K_GNU_HASH_OFFSET;
    hash.sh_size = 4 * sizeof(ours::u32) + sizeof(Traits::Addr) + 3 * sizeof(ours::u32);
    hash.sh_link = 2;
    hash.sh_addralign = alignof(Traits::Addr);

    put(image, K_SECTION_TABLE_OFFSET + sizeof(Section), strings);
    put(image, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), dynamic_symbols);
    put(image, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), hash);

    std::array<u8, 13> const names{0, 'f', 'o', 'o', 0, 'b', 'a', 'r', 0, 'b', 'a', 'z', 0};
    std::memcpy(image.data() + K_STRING_TABLE_OFFSET, names.data(), names.size());
    Symbol foo{};
    foo.st_name = 1;
    put(image, K_DYNSYM_OFFSET + sizeof(Symbol), foo);
    Symbol bar{};
    bar.st_name = 5;
    put(image, K_DYNSYM_OFFSET + 2 * sizeof(Symbol), bar);

    std::array<ours::u32, 4> const header_words{1, 1, 1, 5};
    std::memcpy(image.data() + K_GNU_HASH_OFFSET, header_words.data(), sizeof(header_words));
    constexpr u64 K_BLOOM = 0x0400000010000220ULL;
    put(image, K_GNU_HASH_OFFSET + 4 * sizeof(ours::u32), K_BLOOM);
    constexpr ours::u32 K_BUCKET = 1;
    put(image, K_GNU_HASH_OFFSET + 4 * sizeof(ours::u32) + sizeof(Traits::Addr), K_BUCKET);
    constexpr ours::u32 K_FOO_CHAIN = 0x0b887388U;
    constexpr ours::u32 K_BAR_CHAIN = 0x0b8860bbU;
    put(image, K_GNU_HASH_OFFSET + 5 * sizeof(ours::u32) + sizeof(Traits::Addr), K_FOO_CHAIN);
    put(image, K_GNU_HASH_OFFSET + 6 * sizeof(ours::u32) + sizeof(Traits::Addr), K_BAR_CHAIN);
    return image;
}

auto make_big_endian_gnu_hash_image() -> std::array<u8, 1024> {
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
    strings.sh_size = 13;
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
    hash.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::GnuHash);
    hash.sh_offset = K_GNU_HASH_OFFSET;
    hash.sh_size = 4 * sizeof(ours::u32) + sizeof(Traits::Addr) + 3 * sizeof(ours::u32);
    hash.sh_link = 2;
    hash.sh_addralign = alignof(Traits::Addr);
    put_big_section(image, K_SECTION_TABLE_OFFSET + sizeof(Section), strings);
    put_big_section(image, K_SECTION_TABLE_OFFSET + 2 * sizeof(Section), dynamic_symbols);
    put_big_section(image, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), hash);

    std::array<u8, 13> const names{0, 'f', 'o', 'o', 0, 'b', 'a', 'r', 0, 'b', 'a', 'z', 0};
    std::memcpy(image.data() + K_STRING_TABLE_OFFSET, names.data(), names.size());
    Symbol foo{};
    foo.st_name = 1;
    put_big_symbol(image, K_DYNSYM_OFFSET + sizeof(Symbol), foo);
    Symbol bar{};
    bar.st_name = 5;
    put_big_symbol(image, K_DYNSYM_OFFSET + 2 * sizeof(Symbol), bar);

    std::array<ours::u32, 4> const header_words{1, 1, 1, 5};
    for (usize index = 0; index < header_words.size(); ++index) {
        put_big(image, K_GNU_HASH_OFFSET + index * sizeof(ours::u32), header_words[index]);
    }
    constexpr u64 K_BLOOM = 0x0400000010000220ULL;
    put_big(image, K_GNU_HASH_OFFSET + 4 * sizeof(ours::u32), K_BLOOM);
    constexpr ours::u32 K_BUCKET = 1;
    put_big(image, K_GNU_HASH_OFFSET + 4 * sizeof(ours::u32) + sizeof(Traits::Addr), K_BUCKET);
    constexpr ours::u32 K_FOO_CHAIN = 0x0b887388U;
    constexpr ours::u32 K_BAR_CHAIN = 0x0b8860bbU;
    put_big(image, K_GNU_HASH_OFFSET + 5 * sizeof(ours::u32) + sizeof(Traits::Addr), K_FOO_CHAIN);
    put_big(image, K_GNU_HASH_OFFSET + 6 * sizeof(ours::u32) + sizeof(Traits::Addr), K_BAR_CHAIN);
    return image;
}

auto test_observes_and_looks_up_gnu_hashes() -> int {
    auto const image = make_gnu_hash_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    ustl::Option<eftl::elf::GnuHashTableView<Traits>> hash = ustl::NONE;
    if (parsed.is_ok()) {
        hash = parsed->gnu_hash_table(3);
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
    ustl::Option<bool> baz_maybe = ustl::NONE;
    if (hash.is_some()) {
        baz_maybe = hash->maybe_contains(eftl::ByteRange(baz.data(), baz.size()));
    }
    auto const foo_hash =
        eftl::elf::GnuHashTableView<Traits>::hash(eftl::ByteRange(foo.data(), foo.size()));
    if (hash.is_some() && hash->bucket_count() == 1 && hash->symbol_offset() == 1 &&
        hash->bloom_count() == 1 && hash->bloom_shift() == 5 && hash->bloom_at(0).is_some() &&
        hash->bucket_at(0).is_some() && hash->bucket_at(0).value() == 1 &&
        hash->chain_at(1).is_some() && hash->chain_at(1).value() == 0x0b887388U &&
        foo_index.is_some() && foo_index.value() == 1 && bar_index.is_some() &&
        bar_index.value() == 2 && baz_index.is_none() && baz_maybe.is_some() &&
        !baz_maybe.value() && parsed->gnu_hash_table(2).is_none() && foo_hash.is_some() &&
        foo_hash.value() == 0x0b887389U &&
        eftl::elf::GnuHashTableView<Traits>::hash(eftl::ByteRange(nullptr, 1)).is_none() &&
        hash->lookup(eftl::ByteRange(nullptr, 1)).is_none()) {
        return 0;
    }
    return 1;
}

auto test_observes_big_endian_gnu_hashes() -> int {
    auto const image = make_big_endian_gnu_hash_image();
    auto const parsed = Binary::from_raw(image.data(), image.size());
    ustl::Option<eftl::elf::GnuHashTableView<Traits>> hash = ustl::NONE;
    if (parsed.is_ok()) {
        hash = parsed->gnu_hash_table(3);
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
    if (hash.is_some() && hash->bloom_at(0).is_some() &&
        hash->bloom_at(0).value() == 0x0400000010000220ULL && hash->bucket_at(0).is_some() &&
        hash->bucket_at(0).value() == 1 && hash->chain_at(1).is_some() &&
        hash->chain_at(1).value() == 0x0b887388U && foo_index.is_some() && foo_index.value() == 1 &&
        bar_index.is_some() && bar_index.value() == 2) {
        return 0;
    }
    return 1;
}

auto test_rejects_invalid_gnu_hash_tables() -> int {
    auto wrong_link = make_gnu_hash_image();
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

    auto invalid_bloom_count = make_gnu_hash_image();
    constexpr ours::u32 K_NOT_POWER_OF_TWO = 3;
    put(invalid_bloom_count, K_GNU_HASH_OFFSET + 2 * sizeof(ours::u32), K_NOT_POWER_OF_TWO);
    auto const invalid_bloom_count_result =
        Binary::from_raw(invalid_bloom_count.data(), invalid_bloom_count.size());
    if (invalid_bloom_count_result.is_ok() ||
        invalid_bloom_count_result.unwrap_err() != eftl::elf::ParseError::InvalidHash) {
        return 2;
    }

    auto invalid_bloom_shift = make_gnu_hash_image();
    constexpr ours::u32 K_INVALID_SHIFT = 32;
    put(invalid_bloom_shift, K_GNU_HASH_OFFSET + 3 * sizeof(ours::u32), K_INVALID_SHIFT);
    auto const invalid_bloom_shift_result =
        Binary::from_raw(invalid_bloom_shift.data(), invalid_bloom_shift.size());
    if (invalid_bloom_shift_result.is_ok() ||
        invalid_bloom_shift_result.unwrap_err() != eftl::elf::ParseError::InvalidHash) {
        return 3;
    }

    auto out_of_range_bucket = make_gnu_hash_image();
    constexpr ours::u32 K_INVALID_BUCKET = 3;
    put(out_of_range_bucket, K_GNU_HASH_OFFSET + 4 * sizeof(ours::u32) + sizeof(Traits::Addr),
        K_INVALID_BUCKET);
    auto const out_of_range_bucket_result =
        Binary::from_raw(out_of_range_bucket.data(), out_of_range_bucket.size());
    if (out_of_range_bucket_result.is_ok() ||
        out_of_range_bucket_result.unwrap_err() != eftl::elf::ParseError::InvalidHash) {
        return 4;
    }

    auto unterminated_chain = make_gnu_hash_image();
    constexpr ours::u32 K_UNTERMINATED_CHAIN = 0x0b8860baU;
    put(unterminated_chain, K_GNU_HASH_OFFSET + 6 * sizeof(ours::u32) + sizeof(Traits::Addr),
        K_UNTERMINATED_CHAIN);
    auto const unterminated_chain_result =
        Binary::from_raw(unterminated_chain.data(), unterminated_chain.size());
    if (unterminated_chain_result.is_ok() ||
        unterminated_chain_result.unwrap_err() != eftl::elf::ParseError::InvalidHash) {
        return 5;
    }

    auto wrong_chain_hash = make_gnu_hash_image();
    constexpr ours::u32 K_WRONG_FOO_CHAIN = 0x0b88738aU;
    put(wrong_chain_hash, K_GNU_HASH_OFFSET + 5 * sizeof(ours::u32) + sizeof(Traits::Addr),
        K_WRONG_FOO_CHAIN);
    auto const wrong_chain_hash_result =
        Binary::from_raw(wrong_chain_hash.data(), wrong_chain_hash.size());
    if (wrong_chain_hash_result.is_ok() ||
        wrong_chain_hash_result.unwrap_err() != eftl::elf::ParseError::InvalidHash) {
        return 6;
    }

    auto bloom_false_negative = make_gnu_hash_image();
    constexpr u64 K_EMPTY_BLOOM = 0;
    put(bloom_false_negative, K_GNU_HASH_OFFSET + 4 * sizeof(ours::u32), K_EMPTY_BLOOM);
    auto const bloom_false_negative_result =
        Binary::from_raw(bloom_false_negative.data(), bloom_false_negative.size());
    if (bloom_false_negative_result.is_ok() ||
        bloom_false_negative_result.unwrap_err() != eftl::elf::ParseError::InvalidHash) {
        return 7;
    }

    auto shared_bucket_chain = make_gnu_hash_image();
    std::memcpy(&hash, shared_bucket_chain.data() + K_SECTION_TABLE_OFFSET + 3 * sizeof(Section),
                sizeof(hash));
    hash.sh_size += sizeof(ours::u32);
    put(shared_bucket_chain, K_SECTION_TABLE_OFFSET + 3 * sizeof(Section), hash);
    constexpr ours::u32 K_TWO_BUCKETS = 2;
    put(shared_bucket_chain, K_GNU_HASH_OFFSET, K_TWO_BUCKETS);
    shared_bucket_chain[K_STRING_TABLE_OFFSET + 9] = 'q';
    shared_bucket_chain[K_STRING_TABLE_OFFSET + 10] = 'u';
    shared_bucket_chain[K_STRING_TABLE_OFFSET + 11] = 'x';
    Symbol qux{};
    qux.st_name = 9;
    put(shared_bucket_chain, K_DYNSYM_OFFSET + 2 * sizeof(Symbol), qux);
    constexpr u64 K_SHARED_CHAIN_BLOOM = 0x0000000812000200ULL;
    put(shared_bucket_chain, K_GNU_HASH_OFFSET + 4 * sizeof(ours::u32), K_SHARED_CHAIN_BLOOM);
    constexpr ours::u32 K_SHARED_BUCKET = 1;
    put(shared_bucket_chain, K_GNU_HASH_OFFSET + 4 * sizeof(ours::u32) + sizeof(Traits::Addr),
        K_SHARED_BUCKET);
    put(shared_bucket_chain, K_GNU_HASH_OFFSET + 5 * sizeof(ours::u32) + sizeof(Traits::Addr),
        K_SHARED_BUCKET);
    constexpr ours::u32 K_FOO_CHAIN = 0x0b887388U;
    constexpr ours::u32 K_QUX_CHAIN = 0x0b88a323U;
    put(shared_bucket_chain, K_GNU_HASH_OFFSET + 6 * sizeof(ours::u32) + sizeof(Traits::Addr),
        K_FOO_CHAIN);
    put(shared_bucket_chain, K_GNU_HASH_OFFSET + 7 * sizeof(ours::u32) + sizeof(Traits::Addr),
        K_QUX_CHAIN);
    auto const shared_bucket_chain_result =
        Binary::from_raw(shared_bucket_chain.data(), shared_bucket_chain.size());
    if (shared_bucket_chain_result.is_ok() ||
        shared_bucket_chain_result.unwrap_err() != eftl::elf::ParseError::InvalidHash) {
        return 8;
    }

    auto truncated_table = make_gnu_hash_image();
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
    return 9;
}

auto test_elf32_observes_gnu_hashes() -> int {
    using Traits32 = eftl::elf::Traits32;
    using Binary32 = eftl::elf::FileImpl<Traits32>;
    using Header32 = Traits32::Header;
    using Section32 = Traits32::Section;
    using Symbol32 = Traits32::Symbol;

    constexpr usize K_SECTION_OFFSET = sizeof(Header32);
    constexpr usize K_STRING_OFFSET = 0x100;
    constexpr usize K_DYNSYM_OFFSET_32 = 0x120;
    constexpr usize K_GNU_HASH_OFFSET_32 = 0x160;
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
    hash.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::GnuHash);
    hash.sh_offset = K_GNU_HASH_OFFSET_32;
    hash.sh_size = 7 * sizeof(ours::u32);
    hash.sh_link = 2;
    hash.sh_addralign = alignof(Traits32::Addr);
    put(image, K_SECTION_OFFSET + sizeof(Section32), strings);
    put(image, K_SECTION_OFFSET + 2 * sizeof(Section32), dynamic_symbols);
    put(image, K_SECTION_OFFSET + 3 * sizeof(Section32), hash);

    std::array<u8, 5> const names{0, 'f', 'o', 'o', 0};
    std::memcpy(image.data() + K_STRING_OFFSET, names.data(), names.size());
    Symbol32 foo{};
    foo.st_name = 1;
    put(image, K_DYNSYM_OFFSET_32 + sizeof(Symbol32), foo);

    std::array<ours::u32, 4> const header_words{1, 1, 1, 5};
    std::memcpy(image.data() + K_GNU_HASH_OFFSET_32, header_words.data(), sizeof(header_words));
    constexpr ours::u32 K_BLOOM = 0x10000200U;
    put(image, K_GNU_HASH_OFFSET_32 + 4 * sizeof(ours::u32), K_BLOOM);
    constexpr ours::u32 K_BUCKET = 1;
    put(image, K_GNU_HASH_OFFSET_32 + 5 * sizeof(ours::u32), K_BUCKET);
    constexpr ours::u32 K_CHAIN = 0x0b887389U;
    put(image, K_GNU_HASH_OFFSET_32 + 6 * sizeof(ours::u32), K_CHAIN);

    std::array<u8, 3> const foo_name{'f', 'o', 'o'};
    auto const parsed = Binary32::from_raw(image.data(), image.size());
    ustl::Option<eftl::elf::GnuHashTableView<Traits32>> hash_table = ustl::NONE;
    if (parsed.is_ok()) {
        hash_table = parsed->gnu_hash_table(3);
    }
    ustl::Option<usize> symbol_index = ustl::NONE;
    if (hash_table.is_some()) {
        symbol_index = hash_table->lookup(eftl::ByteRange(foo_name.data(), foo_name.size()));
    }
    if (hash_table.is_some() && hash_table->bloom_at(0).is_some() &&
        hash_table->bloom_at(0).value() == K_BLOOM && symbol_index.is_some() &&
        symbol_index.value() == 1) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_and_looks_up_gnu_hashes(); result != 0) {
        return result;
    }
    if (auto const result = test_rejects_invalid_gnu_hash_tables(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_elf32_observes_gnu_hashes(); result != 0) {
        return 20 + result;
    }
    if (test_observes_big_endian_gnu_hashes() == 0) {
        return 0;
    }
    return 30;
}
