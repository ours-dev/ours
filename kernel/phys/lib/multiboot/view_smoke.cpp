#include <multiboot/view.hpp>

#include <array>

#include <cstdlib>
#include <cstring>

namespace {
using multiboot::MultibootMemoryMap;
using multiboot::MultibootMmapEntry;
using multiboot::MultibootTagMmap;
using multiboot::MultibootTagModule;
using multiboot::View;

struct InfoHeader final {
    u32 total_size;
    u32 reserved;
};

struct TagHeader final {
    u32 type;
    u32 size;
};

struct MemoryMapPayload final {
    u32 entry_size;
    u32 entry_version;
    MultibootMemoryMap entry;
};

auto require(bool const condition) -> void {
    if (!condition) {
        std::abort();
    }
}

auto align_up(usize const value) -> usize {
    return (value + 7) & ~usize{7};
}

auto append_tag(std::array<u8, 128> &bytes, usize cursor, u32 const type, void const *payload,
                usize const payload_size) -> usize {
    TagHeader const header{
        .type = type,
        .size = static_cast<u32>(sizeof(TagHeader) + payload_size),
    };
    std::memcpy(bytes.data() + cursor, &header, sizeof(header));
    if (payload_size != 0) {
        std::memcpy(bytes.data() + cursor + sizeof(header), payload, payload_size);
    }
    return align_up(cursor + header.size);
}

auto finish(std::array<u8, 128> &bytes, usize cursor) -> usize {
    cursor = append_tag(bytes, cursor, 0, nullptr, 0);
    InfoHeader const header{
        .total_size = static_cast<u32>(cursor),
        .reserved = 0,
    };
    std::memcpy(bytes.data(), &header, sizeof(header));
    return cursor;
}

auto add_module(std::array<u8, 128> &bytes, usize const cursor, u32 const start, u32 const end)
    -> usize {
    std::array<u32, 2> const payload{start, end};
    return append_tag(bytes, cursor, 3, payload.data(), sizeof(payload));
}

auto valid_first_module_is_selected() -> void {
    alignas(8) auto bytes = std::array<u8, 128>{};
    auto cursor = sizeof(InfoHeader);
    cursor = add_module(bytes, cursor, 0x0010'0000, 0x0010'8000);
    cursor = finish(bytes, cursor);

    auto const view = View::from(bytes.data(), cursor);
    require(view.is_ok());
    auto const module = view.unwrap().module();
    require(module.is_ok());
    require(module.unwrap().start == 0x0010'0000);
    require(module.unwrap().end == 0x0010'8000);
}

auto malformed_or_ambiguous_modules_are_rejected() -> void {
    alignas(8) auto bytes = std::array<u8, 128>{};
    auto cursor = sizeof(InfoHeader);
    cursor = add_module(bytes, cursor, 0x0010'0000, 0x0010'8000);
    cursor = finish(bytes, cursor);

    auto truncated = bytes;
    auto bad_size = u32{24};
    std::memcpy(truncated.data() + sizeof(InfoHeader) + sizeof(u32), &bad_size, sizeof(bad_size));
    require(View::from(truncated.data(), cursor).is_err());

    alignas(8) auto no_module = std::array<u8, 128>{};
    auto no_module_size = finish(no_module, sizeof(InfoHeader));
    auto const no_module_view = View::from(no_module.data(), no_module_size);
    require(no_module_view.is_ok());
    require(no_module_view.unwrap().module().is_err());

    alignas(8) auto duplicate = std::array<u8, 128>{};
    auto duplicate_cursor = sizeof(InfoHeader);
    duplicate_cursor = add_module(duplicate, duplicate_cursor, 0x0010'0000, 0x0010'8000);
    duplicate_cursor = add_module(duplicate, duplicate_cursor, 0x0020'0000, 0x0020'8000);
    duplicate_cursor = finish(duplicate, duplicate_cursor);
    auto const duplicate_view = View::from(duplicate.data(), duplicate_cursor);
    require(duplicate_view.is_ok());
    require(duplicate_view.unwrap().module().is_err());

    alignas(8) auto empty = std::array<u8, 128>{};
    auto empty_cursor = sizeof(InfoHeader);
    empty_cursor = add_module(empty, empty_cursor, 0x0010'0000, 0x0010'0000);
    empty_cursor = finish(empty, empty_cursor);
    auto const empty_view = View::from(empty.data(), empty_cursor);
    require(empty_view.is_ok());
    require(empty_view.unwrap().module().is_err());
}

auto memory_map_is_typed_and_unique() -> void {
    alignas(8) auto bytes = std::array<u8, 128>{};
    auto cursor = sizeof(InfoHeader);
    cursor = add_module(bytes, cursor, 0x0010'0000, 0x0010'8000);
    auto const map_payload = MemoryMapPayload{
        .entry_size = sizeof(MultibootMemoryMap),
        .entry_version = 0,
        .entry =
            {
                .addr = 0x0020'0000,
                .len = 0x0040'0000,
                .type = MultibootMmapEntry::Available,
                .zero = 0,
            },
    };
    cursor = append_tag(bytes, cursor, 6, &map_payload, sizeof(map_payload));
    cursor = finish(bytes, cursor);

    auto const view = View::from(bytes.data(), cursor);
    require(view.is_ok());
    auto const memory_map = view.unwrap().memory_map();
    require(memory_map.is_ok() && memory_map.unwrap().is_some());
    require(memory_map.unwrap().value().count() == 1);
    auto const range = memory_map.unwrap().value().range(0);
    require(range.addr == 0x0020'0000 && range.len == 0x0040'0000);

    auto bad_version = bytes;
    auto version = u32{1};
    auto constexpr MODULE_TAG_SIZE = sizeof(MultibootTagModule);
    auto constexpr MEMORY_MAP_VERSION_OFFSET = offsetof(MultibootTagMmap, entry_version);
    std::memcpy(bad_version.data() + sizeof(InfoHeader) + MODULE_TAG_SIZE +
                    MEMORY_MAP_VERSION_OFFSET,
                &version, sizeof(version));
    auto const bad_view = View::from(bad_version.data(), cursor);
    require(bad_view.is_ok());
    require(bad_view.unwrap().memory_map().is_err());
}
} // namespace

auto main() -> int {
    valid_first_module_is_selected();
    malformed_or_ambiguous_modules_are_rejected();
    memory_map_is_typed_and_unique();
    return 0;
}
