/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_ALLOC_REQUEST_HPP
#define OURS_MM_PRIV_ALLOC_REQUEST_HPP 1

// CpuNum, NnId and usize come from <ours/types.hpp>, so this does not pull in
// <ours/cpu.hpp>. <ours/mm/types.hpp> is here for ZoneType, which the zone
// range below needs; it is the one heavy include, and the host test builds
// with it the same way the other mm smoke tests do.
#include <ours/mm/gaf.hpp>
#include <ours/mm/node_mask.hpp>
#include <ours/mm/types.hpp>
#include <ours/types.hpp>

namespace ours::mm {
/// One allocation, resolved: what the caller asked for and what the allocator
/// worked out from it.
///
/// Built once, by resolve_request(), and read -- never written -- by every
/// layer below. That is the whole point of it. The walk, the zones and the
/// postamble used to be handed four parameters: a `Gaf`, an `order`, an
/// `AllocationOptions` and a `MemoryAllocationContext`. Three of those were
/// views of one thing and the fourth was an option set that was always empty,
/// so each layer re-derived from them whatever it needed.
///
/// Immutable on purpose, and that is not a style preference. A value one
/// thread owns for the length of one allocation needs no synchronisation at
/// all, which is what lets the zone walk read it outside every lock. The
/// walk's cursor is deliberately not in here: a mutable iteration position
/// inside an otherwise read-only value is what made the shape it replaces hard
/// to reason about.
struct AllocRequest {
    /// What the caller asked for, unchanged.
    Gaf gaf;

    /// Frames, as a power of two. At most MAX_FRAME_ORDER; the public entry
    /// refuses anything larger before a request is built.
    usize order;

    /// The nodes this allocation may draw from.
    ///
    /// Never empty for a request that can succeed: an unset NodeMask answers
    /// false for every node, so the zone walk would reject everything.
    ///
    /// `Gaf::ThisNode` narrows it to the home node whenever the caller's mask
    /// includes it, so the walk then skips every other node's zones even when
    /// the home node has nothing: a restriction, not a preference. Only a mask
    /// that leaves the home node out keeps its own nodes. That is not the
    /// memoryless-node case -- a node with no memory is still in the online
    /// mask, so ThisNode leaves such a node nothing to allocate from.
    NodeMask allowed_nodes;

    /// The node serving this allocation: the one whose zone list the walk
    /// starts from, and what the locality counters compare against.
    NnId home_node;

    /// The CPU the request came from.
    CpuNum initiator;

    /// The zone types this allocation may be served from, both bounds
    /// inclusive.
    ///
    /// Here rather than implied, because zones are not interchangeable: a Dma
    /// zone's frames are addressable by a device that cannot reach higher
    /// memory, and a Normal zone's are not. An interface that lets every
    /// caller treat them as one thing unifies them by pretending the
    /// difference away, and the difference is the only reason zones exist.
    ///
    /// Every caller gets the whole range today because no Gaf bit names a
    /// zone -- the Dma, Dma32 and Movable bits that once did were never passed
    /// by anyone and are gone. What this changes is that the range is a value
    /// on the request, set in one place, rather than a constant pair written
    /// into the walk.
    ZoneType lowest_zone;
    ZoneType highest_zone;
};

/// Resolves a caller's ask into the request every layer below reads.
///
/// Pure: everything it needs is an argument, including the calling CPU, so the
/// same code runs in a host test without a kernel. That is also why it takes
/// |home| rather than reading the current node -- which node serves an
/// allocation is a different question, answered before this one.
FORCE_INLINE
inline auto resolve_request(Gaf const gaf, usize const order, NodeMask const &fallback,
                            NnId const home, CpuNum const initiator) -> AllocRequest {
    auto allowed = fallback;
    if (!!(gaf & Gaf::ThisNode)) {
        auto local = fallback;
        local &= NodeMask::of(home);
        if (local.any()) {
            allowed = local;
        }
    }

    return AllocRequest{
        .gaf = gaf,
        .order = order,
        .allowed_nodes = allowed,
        .home_node = home,
        .initiator = initiator,
        // No Gaf bit names a zone, so every allocation may use Dma through
        // Normal. Movable and Dynamic are above the bound deliberately: a
        // caller that has not said it can take movable memory must not be
        // given it.
        .lowest_zone = ZoneType::Dma,
        .highest_zone = ZoneType::Normal,
    };
}

} // namespace ours::mm

#endif // #ifndef OURS_MM_PRIV_ALLOC_REQUEST_HPP
