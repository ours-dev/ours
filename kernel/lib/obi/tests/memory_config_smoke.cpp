#include <array>
#include <vector>

#include <cstdlib>

#include <obi/container_view.hpp>
#include <obi/memory_config_view.hpp>
#include <obi/package_builder.hpp>

namespace {
using obi::Compression;
using obi::ContainerView;
using obi::ItemFlags;
using obi::ItemFormat;
using obi::ItemSource;
using obi::ItemType;
using obi::MemoryConfigView;
using obi::MemoryRange;
using obi::MemoryRangeType;
using obi::PackageBuilder;

auto require(bool condition) -> void {
    if (!condition) {
        std::abort();
    }
}

// Builds a one-item container and returns the parsed "memory" ItemView.
// The backing container is stashed in a static so it outlives the
// returned view — every call site consumes its result immediately.
auto build_memory_item(ItemType type, obi::ItemView::Bytes payload, u64 memory_size)
    -> ContainerView::ItemResult {
    PackageBuilder builder;
    auto const added = builder.add_item(ItemSource{
        .name = "memory",
        .type = type,
        .format = ItemFormat::MemConfig,
        .data = payload,
        .flags = ItemFlags::Required,
        .memory_size = memory_size,
        .alignment = 8,
        .compression = Compression::None,
    });
    require(added.is_ok());

    static auto container = std::vector<u8>{};
    container = builder.build(/*crc=*/false);
    require(!container.empty());

    auto package = ContainerView::from(container.data(), container.size());
    require(package.is_ok());
    auto memory = package.unwrap().find("memory");
    require(memory.is_ok() && memory.unwrap().is_some());
    return ustl::ok(memory.unwrap().value());
}
} // namespace

auto main() -> int {
    constexpr auto ranges = std::array<MemoryRange, 2>{
        MemoryRange{0x1000, 0x9000, static_cast<u32>(MemoryRangeType::Ram), 0},
        MemoryRange{0xF000, 0x1000, static_cast<u32>(MemoryRangeType::Reserved), 0},
    };
    auto const ranges_bytes =
        obi::ItemView::Bytes(reinterpret_cast<u8 const *>(ranges.data()), sizeof(ranges));

    {
        auto const item = build_memory_item(ItemType::Data, ranges_bytes, sizeof(ranges));
        require(item.is_ok());
        auto view = MemoryConfigView::from(item.unwrap());
        require(view.is_ok() && view.unwrap().count() == 2);
        require(view.unwrap().range(0).is_ok());
        require(view.unwrap().range(2).is_err());
    }

    {
        // An overflowing base+length range must be rejected at range()
        // resolution time, not silently accepted.
        constexpr auto overflowing = MemoryRange{
            0xFFFFFFFFFFFFF000,
            0x2000,
            static_cast<u32>(MemoryRangeType::Ram),
            0,
        };
        auto const item = build_memory_item(
            ItemType::Data,
            obi::ItemView::Bytes(reinterpret_cast<u8 const *>(&overflowing), sizeof(overflowing)),
            sizeof(overflowing));
        require(item.is_ok());
        auto view = MemoryConfigView::from(item.unwrap());
        require(view.is_ok());
        require(view.unwrap().range(0).is_err());
    }

    {
        // Wrong item type (Config instead of Data) must be rejected.
        auto const item = build_memory_item(ItemType::Config, ranges_bytes, sizeof(ranges));
        require(item.is_ok());
        require(MemoryConfigView::from(item.unwrap()).is_err());
    }

    {
        // memconfig doesn't support load-time expansion: memory_size must
        // equal payload_size exactly.
        auto const item = build_memory_item(ItemType::Data, ranges_bytes, sizeof(ranges) + 8);
        require(item.is_ok());
        require(MemoryConfigView::from(item.unwrap()).is_err());
    }

    return 0;
}
