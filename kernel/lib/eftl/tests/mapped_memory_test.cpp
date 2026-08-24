#include <eftl/elf/load_policy.hpp>
#include <eftl/elf/mapped_memory.hpp>

#include <array>
#include <limits>

#include <cstring>

namespace {
auto test_translates_only_within_explicit_load_regions() -> int {
    using Traits = eftl::elf::Traits64;
    using Addr = Traits::Addr;

    alignas(Addr) std::array<u8, 0x200> image{};
    std::array<eftl::elf::LoadRegion, 2> const regions{{
        {.virtual_start = 0x1000,
         .virtual_end = 0x1100,
         .image_offset = 0,
         .permissions = eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Writable},
        {.virtual_start = 0x2000,
         .virtual_end = 0x2100,
         .image_offset = 0x100,
         .permissions = eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Writable},
    }};
    eftl::elf::MappedMemory<Traits> memory(image.data(), image.size(),
                                           ustl::views::Span<eftl::elf::LoadRegion const>(regions));

    constexpr Addr K_FIRST_VALUE = 0x1122334455667788;
    constexpr Addr K_SECOND_VALUE = 0x8877665544332211;
    if (!memory.write<Addr>(0x1008, K_FIRST_VALUE) || !memory.write<Addr>(0x2008, K_SECOND_VALUE)) {
        return 1;
    }
    auto const *const first = memory.read_at<Addr>(0x1008);
    auto const *const second = memory.read_at<Addr>(0x2008);
    if (first == nullptr || second == nullptr || *first != K_FIRST_VALUE ||
        *second != K_SECOND_VALUE) {
        return 2;
    }
    auto const second_bytes = memory.read_at_n<u8>(0x2008, sizeof(Addr));
    if (second_bytes.size() != sizeof(Addr) || second_bytes.data() != image.data() + 0x108) {
        return 3;
    }

    Addr stored_second{};
    std::memcpy(&stored_second, image.data() + 0x108, sizeof(stored_second));
    if (stored_second != K_SECOND_VALUE) {
        return 4;
    }
    if (memory.read_at<Addr>(0x1800) != nullptr || memory.write<Addr>(0x1800, K_FIRST_VALUE)) {
        return 5;
    }
    if (!memory.read_at_n<u16>(0x10fe, 2).empty()) {
        return 6;
    }
    return 0;
}

auto test_rejects_reversed_region_sets() -> int {
    using Traits = eftl::elf::Traits64;

    alignas(Traits::Addr) std::array<u8, 0x20> image{};
    std::array<eftl::elf::LoadRegion, 2> const regions{{
        {.virtual_start = 0x3001,
         .virtual_end = 0x3000,
         .image_offset = 0,
         .permissions = eftl::elf::SegPerms::Writable},
        {.virtual_start = 0x6000,
         .virtual_end = 0x6010,
         .image_offset = 0,
         .permissions = eftl::elf::SegPerms::Writable},
    }};
    eftl::elf::MappedMemory<Traits> memory(image.data(), image.size(),
                                           ustl::views::Span<eftl::elf::LoadRegion const>(regions));

    if (memory.read_at<u8>(0x6000) != nullptr || memory.write<u8>(0x6000, 0x5a)) {
        return 1;
    }
    return 0;
}

auto test_rejects_image_offset_addition_overflow() -> int {
    using Traits = eftl::elf::Traits64;

    std::array<u8, 0x20> image{};
    std::array<eftl::elf::LoadRegion, 1> const regions{{
        {.virtual_start = 0x4000,
         .virtual_end = 0x4010,
         .image_offset = std::numeric_limits<usize>::max()},
    }};
    eftl::elf::MappedMemory<Traits> memory(image.data(), image.size(),
                                           ustl::views::Span<eftl::elf::LoadRegion const>(regions));

    if (memory.read_at<u8>(0x4001) != nullptr || memory.write<u8>(0x4001, 0x5a)) {
        return 1;
    }
    return 0;
}

auto test_rejects_requests_beyond_the_backing_image() -> int {
    using Traits = eftl::elf::Traits64;

    std::array<u8, 0x20> image{};
    std::array<eftl::elf::LoadRegion, 1> const regions{{
        {.virtual_start = 0x5000,
         .virtual_end = 0x5010,
         .image_offset = image.size() - 1,
         .permissions = eftl::elf::SegPerms::Writable},
    }};
    eftl::elf::MappedMemory<Traits> memory(image.data(), image.size(),
                                           ustl::views::Span<eftl::elf::LoadRegion const>(regions));

    if (!memory.read_at_n<u8>(0x5000, 2).empty()) {
        return 1;
    }
    return 0;
}

auto test_rejects_null_backing_with_nonzero_size() -> int {
    using Traits = eftl::elf::Traits64;

    std::array<eftl::elf::LoadRegion, 1> const regions{{
        {.virtual_start = 0x8000,
         .virtual_end = 0x8010,
         .image_offset = 0,
         .permissions = eftl::elf::SegPerms::Writable},
    }};
    eftl::elf::MappedMemory<Traits> memory(nullptr, 1,
                                           ustl::views::Span<eftl::elf::LoadRegion const>(regions));

    if (!memory.read_at_n<u8>(0x8000, 1).empty() ||
        memory.read_at<u8>(0x8000) != nullptr || memory.write<u8>(0x8000, 0x5a)) {
        return 1;
    }
    return 0;
}

auto test_elf32_maps_the_top_byte_through_u64_intervals() -> int {
    using Traits = eftl::elf::Traits32;

    std::array<u8, 1> image{};
    std::array<eftl::elf::LoadRegion, 1> const regions{{
        {.virtual_start = 0xffffffff,
         .virtual_end = 0x100000000,
         .image_offset = 0,
         .permissions = eftl::elf::SegPerms::Writable},
    }};
    eftl::elf::MappedMemory<Traits> memory(image.data(), image.size(),
                                           ustl::views::Span<eftl::elf::LoadRegion const>(regions));

    if (memory.write<u8>(0xffffffff, 0xa5) && image[0] == 0xa5) {
        return 0;
    }
    return 1;
}

auto test_rejects_overlapping_load_regions() -> int {
    using Traits = eftl::elf::Traits64;

    std::array<u8, 0x20> image{};
    std::array<eftl::elf::LoadRegion, 2> const regions{{
        {.virtual_start = 0x7000,
         .virtual_end = 0x7010,
         .image_offset = 0,
         .permissions = eftl::elf::SegPerms::Writable},
        {.virtual_start = 0x7008,
         .virtual_end = 0x7018,
         .image_offset = 8,
         .permissions = eftl::elf::SegPerms::Writable},
    }};
    eftl::elf::MappedMemory<Traits> memory(image.data(), image.size(),
                                           ustl::views::Span<eftl::elf::LoadRegion const>(regions));

    if (memory.read_at<u8>(0x7008) != nullptr || memory.write<u8>(0x7008, 0x5a)) {
        return 1;
    }
    return 0;
}

auto test_rejects_writes_to_readonly_regions() -> int {
    using Traits = eftl::elf::Traits64;

    std::array<u8, 0x10> image{};
    std::array<eftl::elf::LoadRegion, 1> const regions{{
        {.virtual_start = 0x9000,
         .virtual_end = 0x9010,
         .image_offset = 0,
         .permissions = eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Executable},
    }};
    eftl::elf::MappedMemory<Traits> memory(image.data(), image.size(),
                                           ustl::views::Span<eftl::elf::LoadRegion const>(regions));

    if (memory.read_at<u8>(0x9000) != nullptr && !memory.write<u8>(0x9000, 0x5a) &&
        image[0] == 0) {
        return 0;
    }
    return 1;
}
} // namespace

auto main() -> int {
    if (auto const result = test_translates_only_within_explicit_load_regions(); result != 0) {
        return result;
    }
    if (auto const result = test_rejects_reversed_region_sets(); result != 0) {
        return 10 + result;
    }
    if (auto const result = test_rejects_image_offset_addition_overflow(); result != 0) {
        return 20 + result;
    }
    if (auto const result = test_rejects_requests_beyond_the_backing_image(); result != 0) {
        return 30 + result;
    }
    if (auto const result = test_elf32_maps_the_top_byte_through_u64_intervals(); result != 0) {
        return 40 + result;
    }
    if (auto const result = test_rejects_overlapping_load_regions(); result != 0) {
        return 50 + result;
    }
    if (auto const result = test_rejects_null_backing_with_nonzero_size(); result != 0) {
        return 60 + result;
    }
    if (auto const result = test_rejects_writes_to_readonly_regions(); result != 0) {
        return 70 + result;
    }
    return 0;
}
