/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_LAYER_KEYS_HPP
#define OURS_MM_PRIV_LAYER_KEYS_HPP 1

namespace ours::mm {
class PmNode;

namespace priv {
    class Pipeline;
} // namespace priv

namespace test {
    class LayerKeys;
} // namespace test

/// A pass to the node layer under the allocation pipeline: PmNode's
/// alloc_frames() and free_frames(), each of which takes one.
///
/// The pipeline (pmm.cpp) is what makes a frame ready for its owner and gives
/// one back: the thread's allocation state, the order bound, the free-memory
/// gate and the second pass on the way in; the checks of a freed frame, the
/// relief wake for a waiter and the pressure observation on the way out. A
/// caller that reached the node layer directly would skip all of them. So the
/// layer's entries take a key only the pipeline can make, and the tests,
/// through their own door (ours/mm/test/layer_keys.hpp). A key, not a friend:
/// a friend would open every private member; the key opens these calls.
class NodeLayerKey final {
    friend class priv::Pipeline;
    friend class test::LayerKeys;
    NodeLayerKey() = default;
};

/// A pass to the zone layer: PmZone's alloc_frames(), free_frames() and
/// adopt(), each of which takes one.
///
/// The node chooses which zone to ask and whether the zone admits the request
/// (PmZone::admits(), a query anyone may make); the zone only hands out and
/// takes back what its buddy holds. PmNode makes the key for its walk, the
/// pipeline for adopting memory that was never a block, and the tests through
/// their door.
class ZoneLayerKey final {
    friend class PmNode;
    friend class priv::Pipeline;
    friend class test::LayerKeys;
    ZoneLayerKey() = default;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PRIV_LAYER_KEYS_HPP
