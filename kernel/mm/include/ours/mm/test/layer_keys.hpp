/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The tests' door below the allocation pipeline: keys to the node and zone
/// layers (ours/mm/priv/layer_keys.hpp), for a test that has to do what the
/// pipeline will not -- drain a zone below every floor, free a block into its
/// zone frame by frame without waking anyone, walk one node with a request it
/// built itself.
///
/// For kernel tests alone. Nothing stops other code from including this
/// header; the keys guard against reaching the layers by mistake, not on
/// purpose, and a production include of it is the thing a review should ask
/// about.
#ifndef OURS_MM_TEST_LAYER_KEYS_HPP
#define OURS_MM_TEST_LAYER_KEYS_HPP 1

#include <ours/mm/priv/layer_keys.hpp>

namespace ours::mm::test {
class LayerKeys final {
  public:
    [[nodiscard]]
    static auto node() -> NodeLayerKey {
        return NodeLayerKey{};
    }

    [[nodiscard]]
    static auto zone() -> ZoneLayerKey {
        return ZoneLayerKey{};
    }
};

} // namespace ours::mm::test

#endif // #ifndef OURS_MM_TEST_LAYER_KEYS_HPP
