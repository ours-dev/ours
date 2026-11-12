/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_PMM_HPP
#define OURS_MM_PRIV_PMM_HPP 1

#include <ours/mm/alloc_flags.hpp>
#include <ours/mm/pm_frame.hpp>
#include <ours/node_mask.hpp>
#include <ours/types.hpp>

#include <ktl/result.hpp>

namespace ours::mm::priv {
/// The allocation pipeline with the nodes it may draw from given as |nodes|.
///
/// mm::alloc_frames(), mm::alloc_frames_bulk() and mm::alloc_frames_fragment()
/// are these with every online node, and are what a caller uses: which nodes
/// an allocation may use is not a caller's to say, beyond Gaf::ThisNode. The
/// set is an input of the pipeline all the same, and a test of a restriction
/// the public interface cannot ask for (an empty set, a set without the home
/// node) reaches it here.
[[nodiscard]]
auto alloc_frames(Gaf flags, usize order, NodeId home, NodeMask const &nodes)
    -> ktl::Result<PmFrame *>;

[[nodiscard]]
auto alloc_frames_bulk(Gaf flags, usize n, NodeId home, NodeMask const &nodes)
    -> ktl::Result<PmFrame::List>;

[[nodiscard]]
auto alloc_frames_fragment(Gaf flags, usize n, NodeId home, NodeMask const &nodes)
    -> ktl::Result<PmFrame::List>;

} // namespace ours::mm::priv

#endif // #ifndef OURS_MM_PRIV_PMM_HPP
