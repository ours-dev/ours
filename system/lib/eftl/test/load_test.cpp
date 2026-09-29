#include <eftl/elf/load.hpp>

#include <array>

#include <ustl/collections/static_vec.hpp>
#include <ustl/io/direct_memory.hpp>

namespace {
using Traits = eftl::elf::Traits64;

template <typename T>
using OneSegment = ustl::collections::StaticVec<T, 1>;
} // namespace

int main() {
    std::array<u8, 16> bytes{};
    bytes[4] = 0x12;
    bytes[5] = 0x34;
    ustl::io::DirectMemory memory(bytes.data(), bytes.size());

    Traits::Segment segment{};
    segment.p_type = static_cast<eftl::elf::Elf64Word>(eftl::elf::SegType::Load);
    segment.p_offset = 4;
    segment.p_vaddr = 0x1000;
    segment.p_filesz = 2;
    segment.p_memsz = 4;
    segment.p_align = 4;

    eftl::elf::LoadInfo<Traits, OneSegment> info{};
    auto observer = info.make_observer();
    if (!observer.observe(memory, segment, eftl::elf::SegTypeMatcher<eftl::elf::SegType::Load>{})) {
        return 1;
    }
    usize visited = 0;
    if (!info.visit_segments([&](auto const &loadable) {
            ++visited;
            return loadable.file_size == 2 && loadable.va_size == 4 &&
                   loadable.content.size() == 2 && loadable.content[0] == 0x12;
        })) {
        return 2;
    }
    if (visited != 1 || info.virtual_size() != 4) {
        return 3;
    }

    std::array<Traits::Segment, 1> const segment_table{segment};
    eftl::elf::LoadInfo<Traits, OneSegment> decoded_info{};
    if (!eftl::elf::decode_segments(
            memory,
            ustl::views::Span<Traits::Segment const>(segment_table.data(), segment_table.size()),
            decoded_info.make_observer())) {
        return 4;
    }
    if (decoded_info.virtual_size() != 4) {
        return 5;
    }

    if (observer.observe(memory, segment, eftl::elf::SegTypeMatcher<eftl::elf::SegType::Load>{})) {
        return 6;
    }

    eftl::elf::LoadInfo<Traits, OneSegment> invalid_info{};
    auto invalid_observer = invalid_info.make_observer();
    segment.p_filesz = 5;
    if (invalid_observer.observe(memory, segment,
                                 eftl::elf::SegTypeMatcher<eftl::elf::SegType::Load>{})) {
        return 7;
    }
    return 0;
}
