/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_SLAB_HPP
#define OURS_MM_SLAB_HPP 1

#include <ours/mm/gaf.hpp>
#include <ours/mm/pm_frame.hpp>

namespace ours::mm {
class MemCache;

/// Operations on a frame that is currently backing a slab.
///
/// `Slab` is an alias of PageFrame (see pm_frame.hpp); the slab-specific state
/// lives in page::SlabBody and is reached through the frame's role-checked
/// accessors. These are free functions rather than members because the frame
/// type is shared by every role -- putting each role's methods on PageFrame
/// would grow one class without bound.
namespace slab {
    using Body = page::SlabBody;

    /// Claims a fresh frame for |owner| and returns it, or null on failure.
    ///
    /// |size| is the object stride and |next_offset| where inside an object
    /// the free-list pointer lives; both come from the owner's layout.
    auto create(MemCache *owner, void *node_cache, Gaf gaf, usize order, usize size,
                usize next_offset, NnId nid) -> Slab *;

    /// Hands the frame back to the buddy allocator.
    auto destroy(Slab *slab) -> void;

    auto init(Slab *slab, MemCache *owner, void *node_cache, usize size, usize next_offset)
        -> Status;

    auto allocate(Slab *slab, Gaf gaf) -> void *;

    /// Pops one object off the free list, or null if the slab is full.
    auto get_object(Slab *slab) -> void *;

    auto put_object(Slab *slab, void *object) -> void;

    FORCE_INLINE
    auto has_object(Slab const *slab) -> bool {
        return slab->as<Body>().free_head != nullptr;
    }

    FORCE_INLINE
    auto owner(Slab const *slab) -> MemCache * {
        return static_cast<MemCache *>(slab->as<Body>().owner);
    }

    /// True once every object has been handed back.
    FORCE_INLINE
    auto is_empty(Slab const *slab) -> bool {
        return slab->as<Body>().num_inuse == 0;
    }

    /// The NodeCache whose lock protects this slab. Opaque: slab.hpp cannot
    /// name MemCache's private nested type.
    FORCE_INLINE
    auto node_cache(Slab const *slab) -> void * {
        return slab->as<Body>().node_cache;
    }
} // namespace slab

} // namespace ours::mm

#endif // #ifndef OURS_MM_SLAB_HPP
