/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <virtio/virtio.hpp>

#include <algorithm>
#include <optional>

namespace virtio {

auto probe(pci::Config &config, IoArena &arena, std::span<usize const> const owned)
    -> ustl::Result<Found, ours::Status> {
    // The walk's answer, set by the visitor: the claim when one function
    // matched, the arena's refusal when its space ran out mid-walk, and
    // neither meaning NotFound.
    auto found = std::optional<Found>{};
    auto failure = ours::Status::Ok;
    pci::walk_bus0(config, [&](pci::Function const &fn) {
        if (fn.vendor != VIRTIO_VENDOR ||
            (fn.device != VIRTIO_BLK_TRANSITIONAL && fn.device != VIRTIO_BLK_MODERN)) {
            return true;
        }
        if (std::find(owned.begin(), owned.end(), fn.index) != owned.end()) {
            return true;
        }
        // BAR0 must be the legacy I/O BAR: bit 0 set names I/O space, and
        // the base is what the register holds above the two low bits the
        // specification keeps for its own flags.
        auto const bar0 = fn.bars[0];
        if ((bar0 & 0x1) == 0) {
            return true;
        }
        auto io_base = static_cast<u16>(bar0 & 0xFFFC);
        if (io_base == 0) {
            // No base: a function that arrived after firmware's pass. Size
            // the decode, take that many ports from the arena, assign,
            // enable -- the writes that light it up.
            auto const sized = pci::size_bar(config, fn.index, 0);
            if (sized.is_err()) {
                return true;
            }
            auto const at = arena.take(static_cast<u32>(sized->size));
            if (at.is_err()) {
                failure = at.unwrap_err();
                return false;
            }
            pci::assign_bar(config, fn.index, 0, *sized, at.unwrap());
            pci::enable_function(config, fn.index);
            io_base = at.unwrap();
        }
        found = Found{.index = fn.index, .io_base = io_base};
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
} // namespace virtio
