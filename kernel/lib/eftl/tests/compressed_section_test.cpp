/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>

#include <array>
#include <type_traits>

#include <cstddef>
#include <cstring>

namespace {
constexpr usize K_SECTION_TABLE_OFFSET = 0x80;
constexpr usize K_SECTION_DATA_OFFSET = 0x200;

template <typename Value, usize Size>
auto put_scalar(std::array<u8, Size> &image, usize offset, Value value, bool big_endian)
    -> void {
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

template <typename Header, usize Size>
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

template <typename Section, usize Size>
auto put_section(std::array<u8, Size> &image, usize offset, Section const &section,
                 bool big_endian) -> void {
    put_scalar(image, offset + offsetof(Section, sh_name), section.sh_name, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_type), section.sh_type, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_flags), section.sh_flags, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_addr), section.sh_addr, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_offset), section.sh_offset, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_size), section.sh_size, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_link), section.sh_link, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_info), section.sh_info, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_addralign), section.sh_addralign, big_endian);
    put_scalar(image, offset + offsetof(Section, sh_entsize), section.sh_entsize, big_endian);
}

template <typename Compression, usize Size>
auto put_compression_header(std::array<u8, Size> &image, usize offset,
                            Compression const &header, bool big_endian) -> void {
    put_scalar(image, offset + offsetof(Compression, ch_type), header.ch_type, big_endian);
    if constexpr (requires { header.ch_reserved; }) {
        put_scalar(image, offset + offsetof(Compression, ch_reserved), header.ch_reserved,
                   big_endian);
    }
    put_scalar(image, offset + offsetof(Compression, ch_size), header.ch_size, big_endian);
    put_scalar(image, offset + offsetof(Compression, ch_addralign), header.ch_addralign,
               big_endian);
}

template <typename Traits>
auto make_compressed_section_image(bool big_endian = false) -> std::array<u8, 1024> {
    using Compression = typename Traits::Compression;
    using Header = typename Traits::Header;
    using Section = typename Traits::Section;

    std::array<u8, 1024> image{};
    Header header{};
    std::memcpy(header.e_ident, eftl::elf::K_ELF_MAGIC, sizeof(eftl::elf::K_ELF_MAGIC));
    header.e_ident[4] = Traits::K_ELF_CLASS;
    if (big_endian) {
        header.e_ident[5] = 2;
    } else {
        header.e_ident[5] = 1;
    }
    header.e_ident[6] = 1;
    header.e_type = 1;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    header.e_shoff = K_SECTION_TABLE_OFFSET;
    header.e_shentsize = sizeof(Section);
    header.e_shnum = 2;
    put_header(image, header, big_endian);

    Section section{};
    section.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Progbits);
    section.sh_flags = static_cast<decltype(section.sh_flags)>(
        static_cast<u64>(eftl::elf::SectionFlags::Compressed));
    section.sh_offset = K_SECTION_DATA_OFFSET;
    section.sh_size = sizeof(Compression) + 3;
    section.sh_addralign = 1;
    put_section(image, K_SECTION_TABLE_OFFSET + sizeof(Section), section, big_endian);

    Compression compression{};
    compression.ch_type = 1;
    compression.ch_size = 0x12345;
    compression.ch_addralign = 16;
    put_compression_header(image, K_SECTION_DATA_OFFSET, compression, big_endian);
    image[K_SECTION_DATA_OFFSET + sizeof(Compression)] = 0x78;
    image[K_SECTION_DATA_OFFSET + sizeof(Compression) + 1] = 0x9c;
    image[K_SECTION_DATA_OFFSET + sizeof(Compression) + 2] = 0;
    return image;
}

template <typename Traits>
auto test_observes_compression(bool big_endian = false) -> int {
    using File = eftl::elf::FileImpl<Traits>;
    auto const bytes = make_compressed_section_image<Traits>(big_endian);
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    auto const compression = [&]() -> ustl::Option<eftl::elf::SectionCompressionView<Traits>> {
        if (parsed.is_ok()) {
            return parsed->compressed_section(1);
        }
        return ustl::NONE;
    }();
    auto const payload = [&]() -> eftl::ByteRange {
        if (compression.is_some()) {
            return compression->encoded_payload();
        }
        return eftl::ByteRange{};
    }();
    if (compression.is_some() && compression->type() == eftl::elf::CompressionType::Zlib &&
        compression->uncompressed_size() == 0x12345 &&
        compression->uncompressed_alignment() == 16 && payload.size() == 3 &&
        payload.data()[0] == 0x78 && payload.data()[1] == 0x9c) {
        return 0;
    }
    return 1;
}

template <typename Traits>
auto test_rejects_invalid_compression() -> int {
    using Compression = typename Traits::Compression;
    using File = eftl::elf::FileImpl<Traits>;
    using Section = typename Traits::Section;

    auto zero_type = make_compressed_section_image<Traits>();
    put_scalar(zero_type, K_SECTION_DATA_OFFSET + offsetof(Compression, ch_type),
               static_cast<ours::u32>(0), false);
    if (File::from_raw(zero_type.data(), zero_type.size()).is_ok()) {
        return 1;
    }

    auto invalid_alignment = make_compressed_section_image<Traits>();
    put_scalar(invalid_alignment, K_SECTION_DATA_OFFSET + offsetof(Compression, ch_addralign),
               static_cast<decltype(Compression{}.ch_addralign)>(3), false);
    if (File::from_raw(invalid_alignment.data(), invalid_alignment.size()).is_ok()) {
        return 2;
    }

    auto truncated_header = make_compressed_section_image<Traits>();
    put_scalar(truncated_header,
               K_SECTION_TABLE_OFFSET + sizeof(Section) + offsetof(Section, sh_size),
               static_cast<decltype(Section{}.sh_size)>(sizeof(Compression) - 1), false);
    if (File::from_raw(truncated_header.data(), truncated_header.size()).is_ok()) {
        return 3;
    }

    auto allocatable = make_compressed_section_image<Traits>();
    put_scalar(allocatable, K_SECTION_TABLE_OFFSET + sizeof(Section) + offsetof(Section, sh_flags),
               static_cast<decltype(Section{}.sh_flags)>(
                   static_cast<u64>(eftl::elf::SectionFlags::Compressed) |
                   static_cast<u64>(eftl::elf::SectionFlags::Alloc)),
               false);
    if (File::from_raw(allocatable.data(), allocatable.size()).is_ok()) {
        return 4;
    }

    if constexpr (requires { Compression{}.ch_reserved; }) {
        auto reserved = make_compressed_section_image<Traits>();
        put_scalar(reserved, K_SECTION_DATA_OFFSET + offsetof(Compression, ch_reserved),
                   static_cast<decltype(Compression{}.ch_reserved)>(1), false);
        if (File::from_raw(reserved.data(), reserved.size()).is_ok()) {
            return 5;
        }
    }

    auto nobits = make_compressed_section_image<Traits>();
    put_scalar(nobits, K_SECTION_TABLE_OFFSET + sizeof(Section) + offsetof(Section, sh_type),
               static_cast<ours::u32>(eftl::elf::SectionType::Nobits), false);
    if (File::from_raw(nobits.data(), nobits.size()).is_err()) {
        return 0;
    }
    return 6;
}

template <typename Traits>
auto test_preserves_extension_compression_type() -> int {
    using Compression = typename Traits::Compression;
    using File = eftl::elf::FileImpl<Traits>;
    constexpr ours::u32 K_OS_SPECIFIC_TYPE = 0x60000000;

    auto bytes = make_compressed_section_image<Traits>();
    put_scalar(bytes, K_SECTION_DATA_OFFSET + offsetof(Compression, ch_type), K_OS_SPECIFIC_TYPE,
               false);
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    auto const compression = [&]() -> ustl::Option<eftl::elf::SectionCompressionView<Traits>> {
        if (parsed.is_ok()) {
            return parsed->compressed_section(1);
        }
        return ustl::NONE;
    }();
    if (compression.is_some() &&
        static_cast<ours::u32>(compression->type()) == K_OS_SPECIFIC_TYPE) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_compression<eftl::elf::Traits32>(); result != 0) {
        return result;
    }
    if (auto const result = test_observes_compression<eftl::elf::Traits64>(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_observes_compression<eftl::elf::Traits32>(true); result != 0) {
        return 20 + result;
    }
    if (auto const result = test_observes_compression<eftl::elf::Traits64>(true); result != 0) {
        return 30 + result;
    }
    if (auto const result = test_rejects_invalid_compression<eftl::elf::Traits32>(); result != 0) {
        return 40 + result;
    }
    if (auto const result = test_rejects_invalid_compression<eftl::elf::Traits64>(); result != 0) {
        return 50 + result;
    }
    if (auto const result = test_preserves_extension_compression_type<eftl::elf::Traits32>();
        result != 0) {
        return 60 + result;
    }
    if (auto const result = test_preserves_extension_compression_type<eftl::elf::Traits64>();
        result != 0) {
        return 70 + result;
    }
    return 0;
}
