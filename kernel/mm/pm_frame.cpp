#include <gktl/command.hpp>
#include <gktl/counter.hpp>
#include <logz4/log.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/pm_frame.hpp>

namespace ours::mm {
auto PageFrame::dump() const -> void {
    log::info("PmFrame at 0x{:0>8X}:", usize(this));
    log::info("  [role: {:5} |zone: {:6} |section: {} |node: {} |pa: 0x{:0>8X}]", to_string(role()),
              to_string(zone()), secnum(), nid(), frame_to_phys(this));
}

auto to_string(PageFrame::Role role) -> char const * {
    switch (role) {
        case PageFrame::Role::None:
            return "None";
        case PageFrame::Role::Io:
            return "Io";
        case PageFrame::Role::Lru:
            return "Lru";
        case PageFrame::Role::Vmm:
            return "VMM";
        case PageFrame::Role::Pmm:
            return "PMM";
        case PageFrame::Role::Slab:
            return "Slab";
        case PageFrame::Role::Heap:
            return "Heap";

        default:
            return "Anonymous";
    }
}

auto to_string(ZoneType zone) -> char const * {
    switch (zone) {
        case ZoneType::Dma:
            return "DMA";
        case ZoneType::Dma32:
            return "DMA32";
        case ZoneType::Normal:
            return "Normal";
        case ZoneType::Movable:
            return "Movable";
        case ZoneType::Dynamic:
            return "Dynamic";

        default:
            return "Unknown";
    }
}

static auto cmd_dump_frame(int argc, char **argv) -> int {
    return 0;
}
GKTL_COMMAND(FrameDumper, "dump-frame", cmd_dump_frame, "");
} // namespace ours::mm