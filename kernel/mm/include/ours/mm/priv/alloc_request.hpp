/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_ALLOC_REQUEST_HPP
#define OURS_MM_PRIV_ALLOC_REQUEST_HPP 1

// CpuNum, NodeId and usize come from <ours/types.hpp>, so this does not pull in
// <ours/cpu.hpp>. <ours/mm/types.hpp> is here for ZoneType, which the zone
// range below needs; it is the one heavy include, and the host test builds
// with it the same way the other mm smoke tests do.
#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/types.hpp>
#include <ours/node_mask.hpp>
#include <ours/types.hpp>

namespace ours::mm {
class AllocRequest;

/// Where an allocation comes from: the node that is to serve it and the CPU
/// that asked.
///
/// One type for the two because NodeId and CpuNum are both aliases of u32, so
/// as two adjacent parameters they were interchangeable to the compiler, and
/// a transposed call built a request that named a CPU as its home node and a
/// node as its initiator -- silently, and then wrongly for the rest of the
/// allocation: the walk would start from whatever node had the CPU's number
/// and the locality counters would compare against it. Folding the pair into
/// one argument leaves exactly one place where the two can still be confused,
/// the brace that builds an Origin, and both of its writers name the fields so
/// that even that place is checked -- a designated initialiser out of
/// declaration order does not compile.
///
/// A plain record rather than a class with accessors: there is no invariant
/// over the pair, and its readers fill and read it field by field.
struct Origin {
    /// The node serving this allocation. Not necessarily the node |cpu| is
    /// on: a caller may name a node directly, and the thread's memory policy
    /// may choose another.
    NodeId node;

    /// The CPU the request came from.
    CpuNum cpu;
};

/// Resolves a caller's ask into the request every layer below reads.
///
/// Pure: everything it needs is an argument, including the calling CPU, so the
/// same code runs in a host test without a kernel. That is also why it takes
/// |origin| rather than reading the current node and CPU -- which node serves
/// an allocation is a different question, answered before this one.
///
/// The only way to build an AllocRequest from nothing, which is what makes
/// "resolved" true of every request that exists: see the class below.
[[nodiscard]]
inline auto resolve_request(AllocFlags alloc_flags, usize order, NodeMask const &fallback,
                            Origin origin) -> AllocRequest;

/// One allocation, resolved: what the caller asked for and what the allocator
/// worked out from it.
///
/// Built once, by resolve_request(), and read -- never written -- by every
/// layer below. That is the whole point of it. The walk, the zones and the
/// postamble used to be handed four parameters: an `AllocFlags`, an `order`, an
/// `AllocationOptions` and a `MemoryAllocationContext`. Three of those were
/// views of one thing and the fourth was an option set that was always empty,
/// so each layer re-derived from them whatever it needed.
///
/// Immutable, and the type is what says so. A value one thread owns for the
/// length of one allocation needs no synchronisation at all, which is what
/// lets the zone walk read it outside every lock -- but a struct of public
/// fields with a comment saying "immutable on purpose" enforces nothing, and
/// `pmnode.test` was already assigning to two of them. So the state is private
/// and reachable only through accessors, the constructor is private too, and a
/// request that differs from another is derived with with_nodes() or
/// with_zone_range() rather than poked.
///
/// The walk's cursor is deliberately not in here: a mutable iteration position
/// inside an otherwise read-only value is what made the shape it replaces hard
/// to reason about. ZoneSelector holds it.
class AllocRequest final {
    using Self = AllocRequest;

  public:
    /// What the caller asked for, unchanged.
    [[nodiscard]]
    FORCE_INLINE
    constexpr auto flags() const -> AllocFlags {
        return m_alloc_flags;
    }

    /// Frames, as a power of two. At most MAX_FRAME_ORDER; the public entry
    /// refuses anything larger before a request is built.
    [[nodiscard]]
    FORCE_INLINE
    constexpr auto order() const -> usize {
        return m_order;
    }

    /// The nodes this allocation may draw from.
    ///
    /// Never empty for a request that can succeed: an unset NodeMask answers
    /// false for every node, so the zone walk would reject everything.
    ///
    /// `AllocFlags::HomeIfAllowed` restricts the walk to the home node if the
    /// caller's mask includes it. This also applies to an online node with
    /// no memory. A mask that excludes the home node keeps its original nodes.
    [[nodiscard]]
    FORCE_INLINE
    constexpr auto allowed_nodes() const -> NodeMask const & {
        return m_allowed_nodes;
    }

    /// The node serving this allocation: the one whose zone list the walk
    /// starts from, and what the locality counters compare against.
    [[nodiscard]]
    FORCE_INLINE
    constexpr auto home_node() const -> NodeId {
        return m_origin.node;
    }

    /// The CPU the request came from.
    [[nodiscard]]
    FORCE_INLINE
    constexpr auto initiator() const -> CpuNum {
        return m_origin.cpu;
    }

    /// The lowest and the highest zone type this allocation may be served
    /// from, both bounds inclusive.
    ///
    /// Here rather than implied, because zones are not interchangeable: a Dma
    /// zone's frames are addressable by a device that cannot reach higher
    /// memory, and a Normal zone's are not. An interface that lets every
    /// caller treat them as one thing unifies them by pretending the
    /// difference away, and the difference is the only reason zones exist.
    ///
    /// Every caller gets the whole range today because no AllocFlags bit names a
    /// zone -- the Dma, Dma32 and Movable bits that once did were never passed
    /// by anyone and are gone. What this changes is that the range is a value
    /// on the request, set in one place, rather than a constant pair written
    /// into the walk.
    [[nodiscard]]
    FORCE_INLINE
    constexpr auto lowest_zone() const -> ZoneType {
        return m_lowest_zone;
    }

    [[nodiscard]]
    FORCE_INLINE
    constexpr auto highest_zone() const -> ZoneType {
        return m_highest_zone;
    }

    /// This request restricted to |nodes|, as a new value.
    ///
    /// The sanctioned way to vary a resolved request, and the reason the
    /// fields could become private without losing anything: a caller that
    /// needs a different node set gets a second request rather than editing
    /// the first, so the one every layer below reads is still the one
    /// resolve_request() produced. `pmnode.test` is the only caller today,
    /// where each case needs one field of the request moved and the rest left
    /// alone.
    [[nodiscard]]
    FORCE_INLINE
    constexpr auto with_nodes(NodeMask const &nodes) const -> Self {
        auto derived = *this;
        derived.m_allowed_nodes = nodes;
        return derived;
    }

    /// This request restricted to the zone types [|lowest|, |highest|], as a
    /// new value. Both bounds together, because a range with one end moved is
    /// still one fact.
    [[nodiscard]]
    FORCE_INLINE
    constexpr auto with_zone_range(ZoneType const lowest, ZoneType const highest) const -> Self {
        auto derived = *this;
        derived.m_lowest_zone = lowest;
        derived.m_highest_zone = highest;
        return derived;
    }

  private:
    /// resolve_request() is the only thing that can build one from scratch.
    /// Everything else either copies a request or derives one with the two
    /// with_* methods above, both of which start from a request that was
    /// resolved -- so there is no path to an AllocRequest whose fields were
    /// never worked out from a caller's ask.
    friend auto resolve_request(AllocFlags alloc_flags, usize order, NodeMask const &fallback,
                                Origin origin) -> AllocRequest;

    constexpr AllocRequest(AllocFlags const alloc_flags, usize const order,
                           NodeMask const &allowed_nodes, Origin const origin,
                           ZoneType const lowest_zone, ZoneType const highest_zone)
        : m_alloc_flags(alloc_flags)
        , m_order(order)
        , m_allowed_nodes(allowed_nodes)
        , m_origin(origin)
        , m_lowest_zone(lowest_zone)
        , m_highest_zone(highest_zone) {}

    AllocFlags m_alloc_flags;
    usize m_order;
    NodeMask m_allowed_nodes;

    /// Kept whole rather than unpacked into two members, so that the type
    /// which made the call unconfusable is the same one a later reader of this
    /// class sees. home_node() and initiator() are still the way out: nothing
    /// wants the pair back.
    Origin m_origin;

    ZoneType m_lowest_zone;
    ZoneType m_highest_zone;
};

FORCE_INLINE
inline auto resolve_request(AllocFlags const alloc_flags, usize const order,
                            NodeMask const &fallback, Origin const origin) -> AllocRequest {
    auto allowed = fallback;
    if (!!(alloc_flags & AllocFlags::HomeIfAllowed)) {
        auto local = fallback;
        local &= NodeMask::of(origin.node);
        if (local.any()) {
            allowed = local;
        }
    }

    return AllocRequest{
        alloc_flags,
        order,
        allowed,
        origin,
        // No AllocFlags bit names a zone, so every allocation may use Dma through
        // Normal. Movable and Dynamic are above the bound deliberately: a
        // caller that has not said it can take movable memory must not be
        // given it.
        ZoneType::Dma,
        ZoneType::Normal,
    };
}

} // namespace ours::mm

#endif // #ifndef OURS_MM_PRIV_ALLOC_REQUEST_HPP
