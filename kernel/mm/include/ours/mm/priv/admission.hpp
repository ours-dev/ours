/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Whether a zone may give a request a block: the arithmetic of admission,
/// with every input an argument, so that a host test reaches it without a
/// kernel.
#ifndef OURS_MM_PRIV_ADMISSION_HPP
#define OURS_MM_PRIV_ADMISSION_HPP 1

#include <ours/types.hpp>

namespace ours::mm {
/// The two passes an allocation makes over its candidate zones.
///
/// The first holds every request at a zone's Low watermark. When no zone
/// admits it there, memory is short: the allocation asks for reclaim and makes
/// the second pass, which lets the request down to Min, and one that cannot
/// block a quarter further. So Low is where reclaim is woken and Min is the
/// floor; the band between them is used only once reclaim has been asked for.
enum class AdmissionPass : u8 {
    /// Every request stops at Low.
    Low,

    /// A request that may block stops at Min, one that may not at three
    /// quarters of Min.
    Min,
};

/// The frames a zone keeps back from a request in |pass|: it may not take the
/// zone's free count below this. |min| and |low| are the zone's watermarks;
/// |can_block| is whether the caller's context permits blocking.
///
/// A caller that cannot block gets the deeper floor because it has nothing
/// else to do: it cannot wait for memory to come back. One that can block is
/// stopped first, so that the band below Min is left to the callers that have
/// no alternative.
[[nodiscard]]
constexpr auto admission_floor(usize const min, usize const low, AdmissionPass const pass,
                               bool const can_block) -> usize {
    if (pass == AdmissionPass::Low) {
        return low;
    }
    if (can_block) {
        return min;
    }
    return min - min / 4;
}

/// Whether 2^|order| frames can come out of a zone with |free| free frames and
/// leave at least |floor|.
///
/// A count, not a promise: |free| is read without the zone lock, so a yes can
/// be wrong by the time the block is taken (an allocation dips a little below
/// the floor) and a no can be wrong too (the walk tries the next zone). It
/// decides where to try, and the buddy decides whether a block of that order
/// is there.
[[nodiscard]]
constexpr auto admits(usize const free, usize const order, usize const floor) -> bool {
    auto const wanted = usize{1} << order;
    if (free < wanted) {
        return false;
    }
    return free - wanted >= floor;
}

/// The share of a zone held back from a request that could have used a
/// higher zone of the same node: the frames the zones above it manage, up to
/// the highest type the request allows, divided by |ratio|. Zero when the
/// request allows nothing above the zone, or the node has nothing there.
///
/// |managed| is the node's managed frames by zone type, zero for a type it
/// has none of; |own| is the zone's type and |highest| the request's highest,
/// both as indices into it.
///
/// Ordering alone (a node's highest zone first) still lets ordinary requests
/// reach a low zone once every zone above it is short. A low zone is the only
/// memory that can serve a request with an address limit, so it keeps a part
/// that only such a request, or one that has nowhere higher to go, may take.
/// The part grows with the memory above: the more there is that an ordinary
/// request could have used instead, the less reason to let it take the low
/// memory.
[[nodiscard]]
constexpr auto lowmem_reserve(usize const *const managed, usize const count, usize const own,
                              usize const highest, usize const ratio) -> usize {
    usize above = 0;
    for (auto type = own + 1; type <= highest && type < count; ++type) {
        above += managed[type];
    }
    return above / ratio;
}

} // namespace ours::mm

#endif // #ifndef OURS_MM_PRIV_ADMISSION_HPP
