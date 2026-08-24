/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/elf/binary.hpp>

#include <array>
#include <type_traits>

#include <cstddef>
#include <cstring>

namespace {
constexpr usize K_SECTION_TABLE_OFFSET = 0x80;
constexpr usize K_NOTE_OFFSET = 0x200;
constexpr usize K_PROGRAM_HEADER_OFFSET = 0x40;
constexpr ours::u32 K_PROPERTY_TYPE_ONE = 0xc0000000;
constexpr ours::u32 K_PROPERTY_TYPE_TWO = 0xc0000001;

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

template <typename Segment, usize Size>
auto put_segment(std::array<u8, Size> &image, usize offset, Segment const &segment,
                 bool big_endian) -> void {
    put_scalar(image, offset + offsetof(Segment, p_type), segment.p_type, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_flags), segment.p_flags, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_offset), segment.p_offset, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_vaddr), segment.p_vaddr, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_paddr), segment.p_paddr, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_filesz), segment.p_filesz, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_memsz), segment.p_memsz, big_endian);
    put_scalar(image, offset + offsetof(Segment, p_align), segment.p_align, big_endian);
}

constexpr auto align_up(usize value, usize alignment) -> usize {
    return (value + alignment - 1) & ~(alignment - 1);
}

template <typename Traits>
auto make_gnu_property_image(bool big_endian = false) -> std::array<u8, 1024> {
    using Header = typename Traits::Header;
    using Segment = typename Traits::Segment;
    using Section = typename Traits::Section;
    constexpr usize K_ALIGNMENT = sizeof(typename Traits::Addr);
    constexpr usize K_PROPERTY_ONE_SIZE = 4;
    constexpr usize K_PROPERTY_TWO_SIZE = 1;
    constexpr usize K_PROPERTY_ONE_TOTAL =
        align_up(2 * sizeof(ours::u32) + K_PROPERTY_ONE_SIZE, K_ALIGNMENT);
    constexpr usize K_PROPERTY_TWO_TOTAL =
        align_up(2 * sizeof(ours::u32) + K_PROPERTY_TWO_SIZE, K_ALIGNMENT);
    constexpr usize K_DESCRIPTOR_SIZE = K_PROPERTY_ONE_TOTAL + K_PROPERTY_TWO_TOTAL;
    constexpr usize K_DESCRIPTOR_OFFSET = K_NOTE_OFFSET + 2 * sizeof(ours::u32) + 4;

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
    header.e_type = 3;
    header.e_version = 1;
    header.e_ehsize = sizeof(Header);
    header.e_phoff = K_PROGRAM_HEADER_OFFSET;
    header.e_phentsize = sizeof(Segment);
    header.e_phnum = 1;
    header.e_shoff = K_SECTION_TABLE_OFFSET;
    header.e_shentsize = sizeof(Section);
    header.e_shnum = 2;
    put_header(image, header, big_endian);

    Section note{};
    note.sh_type = static_cast<ours::u32>(eftl::elf::SectionType::Note);
    note.sh_offset = K_NOTE_OFFSET;
    note.sh_size = 2 * sizeof(ours::u32) + 4 + K_DESCRIPTOR_SIZE;
    note.sh_addralign = K_ALIGNMENT;
    put_section(image, K_SECTION_TABLE_OFFSET + sizeof(Section), note, big_endian);

    Segment property{};
    property.p_type = static_cast<ours::u32>(eftl::elf::SegType::GnuProperty);
    property.p_offset = K_NOTE_OFFSET;
    property.p_filesz = note.sh_size;
    property.p_memsz = note.sh_size;
    property.p_align = K_ALIGNMENT;
    put_segment(image, K_PROGRAM_HEADER_OFFSET, property, big_endian);

    put_scalar(image, K_NOTE_OFFSET, static_cast<ours::u32>(4), big_endian);
    put_scalar(image, K_NOTE_OFFSET + sizeof(ours::u32), static_cast<ours::u32>(K_DESCRIPTOR_SIZE),
               big_endian);
    put_scalar(image, K_NOTE_OFFSET + 2 * sizeof(ours::u32), static_cast<ours::u32>(5), big_endian);
    image[K_NOTE_OFFSET + 3 * sizeof(ours::u32)] = 'G';
    image[K_NOTE_OFFSET + 3 * sizeof(ours::u32) + 1] = 'N';
    image[K_NOTE_OFFSET + 3 * sizeof(ours::u32) + 2] = 'U';

    put_scalar(image, K_DESCRIPTOR_OFFSET, K_PROPERTY_TYPE_ONE, big_endian);
    put_scalar(image, K_DESCRIPTOR_OFFSET + sizeof(ours::u32), K_PROPERTY_ONE_SIZE, big_endian);
    put_scalar(image, K_DESCRIPTOR_OFFSET + 2 * sizeof(ours::u32), static_cast<ours::u32>(5),
               big_endian);
    auto const second_property = K_DESCRIPTOR_OFFSET + K_PROPERTY_ONE_TOTAL;
    put_scalar(image, second_property, K_PROPERTY_TYPE_TWO, big_endian);
    put_scalar(image, second_property + sizeof(ours::u32), K_PROPERTY_TWO_SIZE, big_endian);
    image[second_property + 2 * sizeof(ours::u32)] = 0xaa;
    return image;
}

template <typename Traits>
auto test_observes_gnu_properties(bool big_endian = false) -> int {
    using File = eftl::elf::FileImpl<Traits>;
    auto const bytes = make_gnu_property_image<Traits>(big_endian);
    auto const parsed = File::from_raw(bytes.data(), bytes.size());
    auto const properties = [&]() -> ustl::Option<eftl::elf::GnuPropertyNoteView<Traits>> {
        if (parsed.is_ok()) {
            return parsed->gnu_property_note(1, 0);
        }
        return ustl::NONE;
    }();
    auto const segment_properties = [&]() -> ustl::Option<eftl::elf::GnuPropertyNoteView<Traits>> {
        if (parsed.is_ok()) {
            return parsed->gnu_property_segment(0, 0);
        }
        return ustl::NONE;
    }();
    auto const first = [&]() -> ustl::Option<eftl::elf::GnuPropertyRecord> {
        if (properties.is_some()) {
            return properties->property_at(0);
        }
        return ustl::NONE;
    }();
    auto const second = [&]() -> ustl::Option<eftl::elf::GnuPropertyRecord> {
        if (properties.is_some()) {
            return properties->property_at(1);
        }
        return ustl::NONE;
    }();
    if (properties.is_some() && segment_properties.is_some() && segment_properties->size() == 2 &&
        properties->size() == 2 && first.is_some() && first->m_type == K_PROPERTY_TYPE_ONE &&
        first->m_data.size() == 4 && first->m_data.data()[0] == 5 && second.is_some() &&
        second->m_type == K_PROPERTY_TYPE_TWO && second->m_data.size() == 1 &&
        second->m_data.data()[0] == 0xaa && properties->property_at(2).is_none()) {
        return 0;
    }
    return 1;
}

template <typename Traits>
auto test_rejects_malformed_gnu_properties() -> int {
    using File = eftl::elf::FileImpl<Traits>;
    using Segment = typename Traits::Segment;
    constexpr usize K_ALIGNMENT = sizeof(typename Traits::Addr);
    constexpr usize K_PROPERTY_ONE_TOTAL = align_up(3 * sizeof(ours::u32), K_ALIGNMENT);
    constexpr usize K_DESCRIPTOR_OFFSET = K_NOTE_OFFSET + 2 * sizeof(ours::u32) + 4;
    auto unsorted = make_gnu_property_image<Traits>();
    put_scalar(unsorted, K_DESCRIPTOR_OFFSET + K_PROPERTY_ONE_TOTAL, K_PROPERTY_TYPE_ONE, false);
    if (File::from_raw(unsorted.data(), unsorted.size()).is_ok()) {
        return 1;
    }

    auto truncated = make_gnu_property_image<Traits>();
    put_scalar(truncated, K_DESCRIPTOR_OFFSET + sizeof(ours::u32), static_cast<ours::u32>(0x100),
               false);
    if (File::from_raw(truncated.data(), truncated.size()).is_ok()) {
        return 2;
    }

    auto empty_property_segment = make_gnu_property_image<Traits>();
    put_scalar(empty_property_segment, K_PROGRAM_HEADER_OFFSET + offsetof(Segment, p_filesz),
               static_cast<decltype(Segment{}.p_filesz)>(0), false);
    if (File::from_raw(empty_property_segment.data(), empty_property_segment.size()).is_err()) {
        return 0;
    }
    return 3;
}
} // namespace

auto main() -> int {
    if (auto const result = test_observes_gnu_properties<eftl::elf::Traits32>(); result != 0) {
        return result;
    }
    if (auto const result = test_observes_gnu_properties<eftl::elf::Traits64>(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_observes_gnu_properties<eftl::elf::Traits32>(true); result != 0) {
        return 20 + result;
    }
    if (auto const result = test_observes_gnu_properties<eftl::elf::Traits64>(true); result != 0) {
        return 30 + result;
    }
    if (auto const result = test_rejects_malformed_gnu_properties<eftl::elf::Traits32>();
        result != 0) {
        return 40 + result;
    }
    if (auto const result = test_rejects_malformed_gnu_properties<eftl::elf::Traits64>();
        result != 0) {
        return 50 + result;
    }
    return 0;
}
