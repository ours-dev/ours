#include <ours/mem/pm-frame.hpp>
#include <ours/mem/memory-model.hpp>

#include <logz4/log.hpp>
#include <gktl/counter.hpp>
#include <gktl/command.hpp>

namespace ours::mem {
    auto PageFrame::dump() const -> void {
        log::info("PmFrame at 0x{:0>8X}:", usize(this));
        log::info("  [role: {:5} |zone: {:6} |section: {} |node: {} |pa: 0x{:0>8X}]",
            to_string(role()),
            to_string(zone()),
            secnum(),
            nid(),
            frame_to_phys(this)
        );
    }

    auto to_string(PageFrame::Role role) -> char const * {
        switch (role) {
            case PageFrame::Role::None:  return "None";
            case PageFrame::Role::Io:    return "Io";
            case PageFrame::Role::Lru:   return "Lru";
            case PageFrame::Role::Vmm:   return "VMM";
            case PageFrame::Role::Pmm:   return "PMM";
            case PageFrame::Role::Slab:  return "Slab";
            case PageFrame::Role::Heap:  return "Heap";

            default: return "Anonymous";
        }
    }

    static auto cmd_dump_frame(int argc, char **argv) -> int {
    }
    GKTL_COMMAND(FrameDumper, "dump-frame", cmd_dump_frame, "");
}