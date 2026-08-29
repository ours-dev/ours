#include <array>

#include <cstdlib>
#include <cstring>

#include <multiboot/memory_map.hpp>

namespace {
using bootmem::MemoryQuery;
using bootmem::MemoryRegion;
using bootmem::MemoryView;
using bootmem::PhysicalMemory;
using bootmem::Range;
using multiboot::MultibootMemoryMap;
using multiboot::MultibootMmapEntry;
using multiboot::View;
using ours::NnId;
using ours::Status;

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

auto require(bool const condition) -> void {
    if (!condition) {
        std::abort();
    }
}

auto align_up(usize const value) -> usize {
    return (value + 7) & ~usize{7};
}

auto append_tag(std::array<u8, 160> &bytes, usize const cursor, u32 const type, void const *payload,
                usize const payload_size) -> usize {
    TagHeader const tag{
        .type = type,
        .size = static_cast<u32>(sizeof(tag) + payload_size),
    };
    std::memcpy(bytes.data() + cursor, &tag, sizeof(tag));
    if (payload_size != 0) {
        std::memcpy(bytes.data() + cursor + sizeof(tag), payload, payload_size);
    }
    return align_up(cursor + tag.size);
}

auto append_module(std::array<u8, 160> &bytes, usize const cursor) -> usize {
    std::array<u32, 2> const module{0x0010'0000, 0x0010'8000};
    return append_tag(bytes, cursor, MULTIBOOT_TAG_TYPE_MODULE, module.data(), sizeof(module));
}

auto append_memory_map(std::array<u8, 160> &bytes, usize const cursor,
                       MultibootMemoryMap const *entries, usize const count) -> usize {
    auto payload = std::array<u8, sizeof(MemoryMapHeader) + 2 * sizeof(MultibootMemoryMap)>{};
    MemoryMapHeader const header{
        .entry_size = sizeof(MultibootMemoryMap),
        .entry_version = 0,
    };
    std::memcpy(payload.data(), &header, sizeof(header));
    std::memcpy(payload.data() + sizeof(header), entries, count * sizeof(*entries));
    return append_tag(bytes, cursor, MULTIBOOT_TAG_TYPE_MMAP, payload.data(),
                      sizeof(header) + count * sizeof(*entries));
}

auto finish(std::array<u8, 160> &bytes, usize cursor) -> usize {
    cursor = append_tag(bytes, cursor, MULTIBOOT_TAG_TYPE_END, nullptr, 0);
    InfoHeader const header{
        .total_size = static_cast<u32>(cursor),
        .reserved = 0,
    };
    std::memcpy(bytes.data(), &header, sizeof(header));
    return cursor;
}

class BootMemFixture final {
  public:
    BootMemFixture() : memory_({.usable = memory_regions_, .reservations = reserved_regions_}) {}

    [[nodiscard]]
    auto memory() -> PhysicalMemory & {
        return memory_;
    }

  private:
    std::array<MemoryRegion, 8> memory_regions_{};
    std::array<Range, 8> reserved_regions_{};
    PhysicalMemory memory_;
};

template <typename Fn>
auto with_parsed_map(std::array<MultibootMemoryMap, 2> const &entries, usize const count, Fn &&fn)
    -> void {
    alignas(8) auto bytes = std::array<u8, 160>{};
    auto cursor = sizeof(InfoHeader);
    cursor = append_module(bytes, cursor);
    cursor = append_memory_map(bytes, cursor, entries.data(), count);
    cursor = finish(bytes, cursor);
    auto const view = View::from(bytes.data(), cursor);
    require(view.is_ok());
    auto const memory_map = view.unwrap().memory_map();
    require(memory_map.is_ok() && memory_map.unwrap().is_some());
    fn(memory_map.unwrap().value());
}

auto reserved_overlap_is_never_allocatable() -> void {
    auto const entries = std::array<MultibootMemoryMap, 2>{
        MultibootMemoryMap{0x1000, 0x8000, MultibootMmapEntry::Available, 0},
        MultibootMemoryMap{0x4000, 0x2000, MultibootMmapEntry::Reserved, 0},
    };
    auto fixture = BootMemFixture{};
    with_parsed_map(entries, entries.size(), [&fixture](View::MemoryMap const &map) {
        require(multiboot::apply_memory_map(map, fixture.memory()) == Status::Ok);
    });

    auto available_size = usize{0};
    auto const status = fixture.memory().visit(
        {
            .lower = 0x1000,
            .upper = 0x9000,
            .node = bootmem::ANY_NODE,
            .view = MemoryView::Free,
        },
        [&available_size](MemoryRegion const range) {
            require(range.range.end() <= 0x4000 || range.range.base >= 0x6000);
            available_size += range.range.size;
            return true;
        });
    require(status == Status::Ok);
    require(available_size == 0x6000);
}

auto no_available_memory_is_rejected() -> void {
    auto const entries = std::array<MultibootMemoryMap, 2>{
        MultibootMemoryMap{0x1000, 0x8000, MultibootMmapEntry::Reserved, 0},
        MultibootMemoryMap{},
    };
    auto fixture = BootMemFixture{};
    with_parsed_map(entries, 1, [&fixture](View::MemoryMap const &map) {
        require(multiboot::apply_memory_map(map, fixture.memory()) == Status::InvalidArgument);
    });
}
} // namespace

auto main() -> int {
    reserved_overlap_is_never_allocatable();
    no_available_memory_is_rejected();
    return 0;
}
