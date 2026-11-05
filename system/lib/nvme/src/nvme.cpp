/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <nvme/nvme.hpp>

#include <algorithm>
#include <optional>

namespace nvme {

auto probe(pci::Config &config, Arena &arena, std::span<usize const> const owned)
    -> ustl::Result<Found, ours::Status> {
    // The walk's answer, set by the visitor: the claim when one function
    // matched, the arena's refusal when its space ran out mid-walk, and
    // neither meaning NotFound.
    auto found = std::optional<Found>{};
    auto failure = ours::Status::Ok;
    pci::walk_bus0(config, [&](pci::Function const &fn) {
        // The class names the family; the interface byte (the low 8 bits)
        // is the controller's own, and every one of them is driven alike.
        if ((fn.class_code >> 8) != (CLASS_NVM >> 8)) {
            return true;
        }
        if (std::find(owned.begin(), owned.end(), fn.index) != owned.end()) {
            return true;
        }
        // BAR0 must be a 64-bit memory BAR: bit 0 clear names memory, bits
        // 2-1 = 10b the 64-bit width. Anything else is not a register file
        // this core knows how to map.
        auto const bar_low = fn.bars[0];
        if ((bar_low & 0x1) != 0 || (bar_low & 0x6) != 0x4) {
            return true;
        }
        auto base = ours::PhysAddr{(u64{fn.bars[1]} << 32) | u64{bar_low & ~u32{0xF}}};
        if (std::to_underlying(base) == 0) {
            // No base: a function that arrived after firmware's pass. Size
            // the decode, take that much from the arena, assign, enable --
            // the writes that light it up.
            auto const sized = pci::size_bar(config, fn.index, 0);
            if (sized.is_err()) {
                return true;
            }
            auto const at = arena.take(sized->size);
            if (at.is_err()) {
                failure = at.unwrap_err();
                return false;
            }
            pci::assign_bar(config, fn.index, 0, *sized, std::to_underlying(at.unwrap()));
            pci::enable_function(config, fn.index);
            base = at.unwrap();
        }
        found = Found{.index = fn.index, .bar0 = base};
        return false;
    });
    if (failure != ours::Status::Ok) {
        return ustl::err(failure);
    }
    if (!found.has_value()) {
        return ustl::err(ours::Status::NotFound);
    }
    return ustl::ok(*found);
}
} // namespace nvme
