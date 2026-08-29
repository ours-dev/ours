/// Host tests for the machine-independent half of a boot-shim.
///
/// Everything here goes through multiboot::View rather than reaching into
/// MemoryMap directly, because MemoryMap has no public constructor -- the
/// only way to get one is to parse a Multiboot2 information block, so these
/// tests synthesise one the same way //kernel/phys/lib/multiboot's own
/// smoke test does.

#include <array>
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <bootshim/legacy_memory.hpp>
#include <multiboot/view.hpp>

namespace {
using bootshim::LegacyMemoryRanges;
using bootshim::MAX_LEGACY_MEMORY_RANGES;
using multiboot::MultibootMemoryMap;
using multiboot::MultibootMmapEntry;
using multiboot::View;

auto require_at(bool const condition, char const *const expression, int const line) -> void {
    if (!condition) {
        // Name the failure: these cases are dense arithmetic over addresses,
        // and a bare abort() leaves nothing to go on.
        std::fprintf(stderr, "legacy_memory_smoke.cpp:%d: require(%s) failed\n", line, expression);
        std::abort();
    }
}

#define require(condition) require_at((condition), #condition, __LINE__)

struct InfoHeader final {
    u32 total_size;
    u32 reserved;
};

struct TagHeader final {
    u32 type;
    u32 size;
};

struct MemoryMapHeader final {
    u32 entry_size;
    u32 entry_version;
};

auto align_up(usize const value) -> usize {
    return (value + 7) & ~usize{7};
}

auto append_tag(std::vector<u8> &bytes, u32 const type, void const *payload,
                usize const payload_size) -> void {
    TagHeader const tag{
        .type = type,
        .size = static_cast<u32>(sizeof(tag) + payload_size),
    };
    auto const start = bytes.size();
    bytes.resize(align_up(start + tag.size));
    std::memcpy(bytes.data() + start, &tag, sizeof(tag));
    if (payload_size != 0) {
        std::memcpy(bytes.data() + start + sizeof(tag), payload, payload_size);
    }
}

/// A Multiboot2 information block carrying exactly the one module tag a
/// shim expects plus `entries` as its memory map.
auto build_info_block(std::vector<MultibootMemoryMap> const &entries) -> std::vector<u8> {
    std::vector<u8> bytes(sizeof(InfoHeader));

    std::array<u32, 2> const module{0x0010'0000, 0x0010'8000};
    append_tag(bytes, MULTIBOOT_TAG_TYPE_MODULE, module.data(), sizeof(module));

    std::vector<u8> payload(sizeof(MemoryMapHeader) + entries.size() * sizeof(MultibootMemoryMap));
    MemoryMapHeader const header{
        .entry_size = sizeof(MultibootMemoryMap),
        .entry_version = 0,
    };
    std::memcpy(payload.data(), &header, sizeof(header));
    if (!entries.empty()) {
        std::memcpy(payload.data() + sizeof(header), entries.data(),
                    entries.size() * sizeof(MultibootMemoryMap));
    }
    append_tag(bytes, MULTIBOOT_TAG_TYPE_MMAP, payload.data(), payload.size());

    append_tag(bytes, MULTIBOOT_TAG_TYPE_END, nullptr, 0);

    InfoHeader const info{
        .total_size = static_cast<u32>(bytes.size()),
        .reserved = 0,
    };
    std::memcpy(bytes.data(), &info, sizeof(info));
    return bytes;
}

template <typename Fn>
auto with_parsed_map(std::vector<MultibootMemoryMap> const &entries, Fn &&fn) -> void {
    auto const bytes = build_info_block(entries);
    auto const view = View::from(bytes.data(), bytes.size());
    require(view.is_ok());
    auto const memory_map = view.unwrap().memory_map();
    require(memory_map.is_ok() && memory_map.unwrap().is_some());
    fn(memory_map.unwrap().value());
}

auto available(u64 const base, u64 const length) -> MultibootMemoryMap {
    return {base, length, MultibootMmapEntry::Available, 0};
}

auto reserved(u64 const base, u64 const length) -> MultibootMemoryMap {
    return {base, length, MultibootMmapEntry::Reserved, 0};
}

// ---------------------------------------------------------------------
// to_obi_memory_type
// ---------------------------------------------------------------------

auto only_available_is_ram() -> void {
    using bootshim::to_obi_memory_type;
    require(to_obi_memory_type(MultibootMmapEntry::Available) == obi::MemoryRangeType::Ram);

    // Every other flavour of unusable folds to Reserved: BootMem only ever
    // asks "may I allocate here", and all of these answer no.
    require(to_obi_memory_type(MultibootMmapEntry::Reserved) == obi::MemoryRangeType::Reserved);
    require(to_obi_memory_type(MultibootMmapEntry::AcpiReclaimable) ==
            obi::MemoryRangeType::Reserved);
    require(to_obi_memory_type(MultibootMmapEntry::Nvs) == obi::MemoryRangeType::Reserved);
    require(to_obi_memory_type(MultibootMmapEntry::BadRam) == obi::MemoryRangeType::Reserved);

    // Including a type this Multiboot2 revision does not define at all --
    // an unknown range is not one to allocate out of.
    require(to_obi_memory_type(0) == obi::MemoryRangeType::Reserved);
    require(to_obi_memory_type(9999) == obi::MemoryRangeType::Reserved);
}

// ---------------------------------------------------------------------
// convert_memory_map
// ---------------------------------------------------------------------

auto conversion_preserves_ranges() -> void {
    auto const entries = std::vector<MultibootMemoryMap>{
        available(0x1000, 0x8000),
        reserved(0xA000, 0x1000),
        {0xB000, 0x2000, MultibootMmapEntry::AcpiReclaimable, 0},
    };
    with_parsed_map(entries, [](View::MemoryMap const &map) {
        LegacyMemoryRanges out{};
        auto const result = bootshim::convert_memory_map(map, out);
        require(result.converted == 3);
        require(result.dropped == 0);

        require(out[0].base == 0x1000 && out[0].length == 0x8000);
        require(out[0].type == static_cast<u32>(obi::MemoryRangeType::Ram));
        require(out[0].reserved == 0);

        require(out[1].base == 0xA000 && out[1].length == 0x1000);
        require(out[1].type == static_cast<u32>(obi::MemoryRangeType::Reserved));

        require(out[2].base == 0xB000 && out[2].length == 0x2000);
        require(out[2].type == static_cast<u32>(obi::MemoryRangeType::Reserved));
    });
}

auto empty_map_converts_to_nothing() -> void {
    with_parsed_map({}, [](View::MemoryMap const &map) {
        require(map.count() == 0);
        LegacyMemoryRanges out{};
        auto const result = bootshim::convert_memory_map(map, out);
        require(result.converted == 0);
        require(result.dropped == 0);
    });
}

auto exactly_full_map_drops_nothing() -> void {
    std::vector<MultibootMemoryMap> entries;
    for (usize index = 0; index < MAX_LEGACY_MEMORY_RANGES; ++index) {
        entries.push_back(available(0x1000 * (index + 1), 0x800));
    }
    with_parsed_map(entries, [](View::MemoryMap const &map) {
        require(map.count() == MAX_LEGACY_MEMORY_RANGES);
        LegacyMemoryRanges out{};
        auto const result = bootshim::convert_memory_map(map, out);
        require(result.converted == MAX_LEGACY_MEMORY_RANGES);
        require(result.dropped == 0);
        require(out[MAX_LEGACY_MEMORY_RANGES - 1].base == 0x1000 * MAX_LEGACY_MEMORY_RANGES);
    });
}

auto oversized_map_reports_what_it_dropped() -> void {
    constexpr auto EXTRA = usize{7};
    std::vector<MultibootMemoryMap> entries;
    for (usize index = 0; index < MAX_LEGACY_MEMORY_RANGES + EXTRA; ++index) {
        entries.push_back(available(0x1000 * (index + 1), 0x800));
    }
    with_parsed_map(entries, [](View::MemoryMap const &map) {
        LegacyMemoryRanges out{};
        auto const result = bootshim::convert_memory_map(map, out);
        require(result.converted == MAX_LEGACY_MEMORY_RANGES);
        require(result.dropped == EXTRA);

        // Conversion is in map order, so what survives is the head of the
        // firmware's own list -- not a selection of the useful ranges.
        require(out[0].base == 0x1000);
        require(out[MAX_LEGACY_MEMORY_RANGES - 1].base == 0x1000 * MAX_LEGACY_MEMORY_RANGES);
    });
}

// ---------------------------------------------------------------------
// find_scratch
// ---------------------------------------------------------------------

constexpr auto PAGE = usize{0x1000};

auto scratch_takes_the_first_region_that_fits() -> void {
    auto const entries = std::vector<MultibootMemoryMap>{
        available(0x10000, 0x4000),
        available(0x80000, 0x40000),
    };
    with_parsed_map(entries, [](View::MemoryMap const &map) {
        auto const found = bootshim::find_scratch(map, 0, 0x2000, PAGE);
        require(found.is_some() && found.value() == 0x10000);
    });
}

auto scratch_skips_regions_that_are_not_available() -> void {
    auto const entries = std::vector<MultibootMemoryMap>{
        // Big enough, wrong type: a shim placing an image in ACPI NVS or
        // firmware-reserved memory is exactly the bug this guards.
        reserved(0x10000, 0x100000),
        {0x200000, 0x100000, MultibootMmapEntry::AcpiReclaimable, 0},
        available(0x400000, 0x10000),
    };
    with_parsed_map(entries, [](View::MemoryMap const &map) {
        auto const found = bootshim::find_scratch(map, 0, 0x8000, PAGE);
        require(found.is_some() && found.value() == 0x400000);
    });
}

auto scratch_skips_regions_that_are_too_small() -> void {
    auto const entries = std::vector<MultibootMemoryMap>{
        available(0x10000, 0x1000),
        available(0x20000, 0x1000),
        available(0x30000, 0x20000),
    };
    with_parsed_map(entries, [](View::MemoryMap const &map) {
        auto const found = bootshim::find_scratch(map, 0, 0x8000, PAGE);
        require(found.is_some() && found.value() == 0x30000);
    });
}

auto scratch_starts_at_the_floor_inside_a_region() -> void {
    auto const entries = std::vector<MultibootMemoryMap>{available(0x10000, 0x100000)};
    with_parsed_map(entries, [](View::MemoryMap const &map) {
        // The floor lands inside the region: the placement moves up to it
        // rather than taking the region's own base.
        auto const found = bootshim::find_scratch(map, 0x40000, 0x1000, PAGE);
        require(found.is_some() && found.value() == 0x40000);
    });
}

auto scratch_aligns_the_floor_up() -> void {
    auto const entries = std::vector<MultibootMemoryMap>{available(0x10000, 0x100000)};
    with_parsed_map(entries, [](View::MemoryMap const &map) {
        // An unaligned floor -- the end of some image the caller must not
        // overwrite -- rounds up, never down.
        auto const found = bootshim::find_scratch(map, 0x40001, 0x1000, PAGE);
        require(found.is_some() && found.value() == 0x41000);

        // A larger alignment rounds further.
        auto const aligned = bootshim::find_scratch(map, 0x40001, 0x1000, 0x10000);
        require(aligned.is_some() && aligned.value() == 0x50000);
    });
}

auto scratch_rejects_a_region_the_floor_has_eaten() -> void {
    auto const entries = std::vector<MultibootMemoryMap>{
        // Large enough on its own, but the floor leaves too little of it.
        available(0x10000, 0x10000),
        available(0x100000, 0x10000),
    };
    with_parsed_map(entries, [](View::MemoryMap const &map) {
        auto const found = bootshim::find_scratch(map, 0x1F000, 0x8000, PAGE);
        require(found.is_some() && found.value() == 0x100000);
    });
}

auto scratch_rejects_a_region_alignment_has_eaten() -> void {
    // [0x10001, 0x18000) holds 0x7FFF raw bytes, but the first page-aligned
    // address in it is 0x11000, which leaves only 0x7000.  A shim that
    // checked the region's length and then aligned would hand back a range
    // running 0x800 bytes past the end of usable memory.
    auto const entries = std::vector<MultibootMemoryMap>{available(0x10001, 0x7FFF)};
    with_parsed_map(entries, [](View::MemoryMap const &map) {
        require(bootshim::find_scratch(map, 0, 0x7800, PAGE).is_none());
        // The largest request the aligned start can still satisfy.
        auto const fits = bootshim::find_scratch(map, 0, 0x7000, PAGE);
        require(fits.is_some() && fits.value() == 0x11000);
    });
}

auto scratch_fails_when_the_floor_is_above_everything() -> void {
    auto const entries = std::vector<MultibootMemoryMap>{
        available(0x10000, 0x10000),
        available(0x30000, 0x10000),
    };
    with_parsed_map(entries, [](View::MemoryMap const &map) {
        require(bootshim::find_scratch(map, 0x100000, 0x1000, PAGE).is_none());
    });
}

auto scratch_fails_when_nothing_is_big_enough() -> void {
    auto const entries = std::vector<MultibootMemoryMap>{
        available(0x10000, 0x1000),
        available(0x20000, 0x1000),
    };
    with_parsed_map(entries, [](View::MemoryMap const &map) {
        require(bootshim::find_scratch(map, 0, 0x8000, PAGE).is_none());
    });
}

auto scratch_fails_on_an_empty_map() -> void {
    with_parsed_map({}, [](View::MemoryMap const &map) {
        require(bootshim::find_scratch(map, 0, 0x1000, PAGE).is_none());
    });
}

auto scratch_rejects_an_unusable_alignment() -> void {
    auto const entries = std::vector<MultibootMemoryMap>{available(0x10000, 0x100000)};
    with_parsed_map(entries, [](View::MemoryMap const &map) {
        // Rounding a non-power-of-two "alignment" to something else would
        // hand back a range the caller's assumptions do not hold for.
        require(bootshim::find_scratch(map, 0, 0x1000, 3).is_none());
        require(bootshim::find_scratch(map, 0, 0x1000, 0).is_none());

        // 1 is a power of two and means "no alignment at all".
        auto const unaligned = bootshim::find_scratch(map, 0x10001, 0x1000, 1);
        require(unaligned.is_some() && unaligned.value() == 0x10001);
    });
}

auto scratch_fits_a_region_exactly() -> void {
    auto const entries = std::vector<MultibootMemoryMap>{
        available(0x10000, 0x10000),
    };
    with_parsed_map(entries, [](View::MemoryMap const &map) {
        // A request that consumes the region down to its last byte is a fit,
        // not an overrun.
        auto const found = bootshim::find_scratch(map, 0, 0x10000, PAGE);
        require(found.is_some() && found.value() == 0x10000);
        // One byte more is not.
        require(bootshim::find_scratch(map, 0, 0x10001, PAGE).is_none());
    });
}

} // namespace

auto main() -> int {
    only_available_is_ram();

    conversion_preserves_ranges();
    empty_map_converts_to_nothing();
    exactly_full_map_drops_nothing();
    oversized_map_reports_what_it_dropped();

    scratch_takes_the_first_region_that_fits();
    scratch_skips_regions_that_are_not_available();
    scratch_skips_regions_that_are_too_small();
    scratch_starts_at_the_floor_inside_a_region();
    scratch_aligns_the_floor_up();
    scratch_rejects_a_region_the_floor_has_eaten();
    scratch_rejects_a_region_alignment_has_eaten();
    scratch_fails_when_the_floor_is_above_everything();
    scratch_fails_when_nothing_is_big_enough();
    scratch_fails_on_an_empty_map();
    scratch_rejects_an_unusable_alignment();
    scratch_fits_a_region_exactly();
    return 0;
}
