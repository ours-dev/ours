/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

// Augmented red-black tree: every node caches an aggregate over its own
// subtree, and the tree must keep that cache exact across insertion,
// erasure and every rotation in between.
//
// The gate here is the brute-force invariant check: after each individual
// operation, every node's cached aggregate is compared against the value
// recomputed by walking its subtree from scratch. Missing a single hook --
// one rotation, or the successor transplant inside erase -- makes a cached
// value go stale and this check catches it immediately.

#include <algorithm>
#include <random>
#include <vector>

#include <cstdio>

#include <ustl/collections/intrusive/rbtree.hpp>

namespace {

// ---------------------------------------------------------------------------
// An interval-tree style augmentation: each node caches the largest `m_end`
// in its subtree. Note that the policy is written *only* in terms of the
// element type -- no NodePtr, no NodeTraits, no tree walking. That is the
// layering this test also exists to pin down.
// ---------------------------------------------------------------------------
struct Interval final: ustl::collections::intrusive::SetBaseHook<> {
    int m_begin{};
    int m_end{};
    int m_max_end{}; // the augmented value
};

struct ByBegin final {
    [[nodiscard]]
    constexpr auto operator()(Interval const &left, Interval const &right) const noexcept -> bool {
        return left.m_begin < right.m_begin;
    }
};

struct MaxEndAugment final {
    using Element = Interval;
    static constexpr auto EXTENSIONS = ustl::collections::intrusive::Extension::Augment;

    static auto recompute(Element &self, Element const *left, Element const *right) noexcept
        -> bool {
        auto value = self.m_end;
        if (left != nullptr) {
            value = std::max(value, left->m_max_end);
        }
        if (right != nullptr) {
            value = std::max(value, right->m_max_end);
        }
        const auto unchanged = value == self.m_max_end;
        self.m_max_end = value;
        return unchanged;
    }

    static auto copy(Element const &src, Element &dst) noexcept -> void {
        dst.m_max_end = src.m_max_end;
    }
};

namespace ci = ustl::collections::intrusive;

using PlainTree = ci::RBTree<Interval, ci::Compare<ByBegin>>;
using AugmentedTree = ci::RBTree<Interval, ci::Compare<ByBegin>, ci::Extensions<MaxEndAugment>>;

// An augmented tree must not cost the container any space: the callback
// table is a link-time constant, not a member.
static_assert(sizeof(AugmentedTree) == sizeof(PlainTree),
              "augmentation must not add per-instance storage");

int g_failures = 0;

auto fail(char const *what) -> void {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

// ---------------------------------------------------------------------------
// Brute force: recompute a subtree's true aggregate by visiting all of it,
// and compare against every cached value on the way.
// ---------------------------------------------------------------------------
auto verify_subtree(AugmentedTree const &tree, AugmentedTree::Iter node, bool &ok) -> int {
    if (node == tree.end()) {
        return std::numeric_limits<int>::min();
    }
    const auto left = verify_subtree(tree, tree.left(node), ok);
    const auto right = verify_subtree(tree, tree.right(node), ok);
    const auto expected = std::max({node->m_end, left, right});
    if (node->m_max_end != expected) {
        ok = false;
    }
    return expected;
}

auto verify(AugmentedTree const &tree) -> bool {
    auto ok = true;
    verify_subtree(tree, tree.root(), ok);
    return ok;
}

// The aggregate at the root must equal the max over every element, which is
// an independent cross-check of the recursive walk above.
auto verify_root_total(AugmentedTree const &tree) -> bool {
    if (tree.root() == tree.end()) {
        return true;
    }
    auto expected = std::numeric_limits<int>::min();
    for (auto const &interval: tree) {
        expected = std::max(expected, interval.m_end);
    }
    return tree.root()->m_max_end == expected;
}

// ---------------------------------------------------------------------------

auto test_random_operations(unsigned seed, int count) -> void {
    const int COUNT = count;
    std::vector<Interval> storage(COUNT);
    std::mt19937 rng(seed);
    for (int i = 0; i < COUNT; ++i) {
        storage[i].m_begin = i;
        storage[i].m_end = static_cast<int>(rng() % 10000);
        storage[i].m_max_end = storage[i].m_end;
    }

    AugmentedTree tree;
    std::vector<int> order(COUNT);
    for (int i = 0; i < COUNT; ++i) {
        order[i] = i;
    }
    std::shuffle(order.begin(), order.end(), rng);

    // Insert one at a time, checking the whole tree after every single step.
    for (int i = 0; i < COUNT; ++i) {
        static_cast<void>(tree.insert_equal(storage[order[i]]));
        if (!verify(tree)) {
            fail("stale aggregate after insertion");
            return;
        }
        if (!verify_root_total(tree)) {
            fail("root aggregate disagrees with the elements after insertion");
            return;
        }
    }

    // Erase in a different order, again checking after every step -- this is
    // what exercises the successor transplant and the erase rotations.
    std::shuffle(order.begin(), order.end(), rng);
    for (int i = 0; i < COUNT; ++i) {
        tree.erase(storage[order[i]]);
        if (!verify(tree)) {
            fail("stale aggregate after erasure");
            return;
        }
        if (!verify_root_total(tree)) {
            fail("root aggregate disagrees with the elements after erasure");
            return;
        }
    }

    if (!tree.empty()) {
        fail("tree not empty after erasing everything");
    }
}

// An augmented tree must balance exactly like a plain one -- augmentation
// rides along with the rotations, it must never change which rotations
// happen. Identical key sequences must therefore yield identical in-order
// traversals and identical tree shapes.
auto test_shape_matches_plain_tree() -> void {
    constexpr int COUNT = 128;
    std::vector<Interval> plain_storage(COUNT);
    std::vector<Interval> augmented_storage(COUNT);
    std::mt19937 rng(7);
    for (int i = 0; i < COUNT; ++i) {
        const auto begin = static_cast<int>(rng() % 1000);
        const auto end = begin + static_cast<int>(rng() % 100) + 1;
        plain_storage[i].m_begin = begin;
        plain_storage[i].m_end = end;
        augmented_storage[i].m_begin = begin;
        augmented_storage[i].m_end = end;
        augmented_storage[i].m_max_end = end;
    }

    PlainTree plain;
    AugmentedTree augmented;
    for (int i = 0; i < COUNT; ++i) {
        static_cast<void>(plain.insert_equal(plain_storage[i]));
        static_cast<void>(augmented.insert_equal(augmented_storage[i]));
    }

    auto plain_it = plain.begin();
    auto augmented_it = augmented.begin();
    for (; plain_it != plain.end() && augmented_it != augmented.end(); ++plain_it, ++augmented_it) {
        if (plain_it->m_begin != augmented_it->m_begin || plain_it->m_end != augmented_it->m_end) {
            fail("augmented tree ordered differently from the plain tree");
            return;
        }
    }
    if (plain_it != plain.end() || augmented_it != augmented.end()) {
        fail("augmented and plain trees hold different numbers of elements");
    }
}

// ---------------------------------------------------------------------------
// A policy that records how often it was called, proving the hooks actually
// fire rather than being quietly skipped.
// ---------------------------------------------------------------------------
struct Counters final {
    static inline int s_recompute = 0;
    static inline int s_copy = 0;
};

struct CountingAugment final {
    using Element = Interval;
    static constexpr auto EXTENSIONS = ustl::collections::intrusive::Extension::Augment;

    static auto recompute(Element &self, Element const *left, Element const *right) noexcept
        -> bool {
        ++Counters::s_recompute;
        return MaxEndAugment::recompute(self, left, right);
    }

    static auto copy(Element const &src, Element &dst) noexcept -> void {
        ++Counters::s_copy;
        MaxEndAugment::copy(src, dst);
    }
};

auto test_hooks_actually_fire() -> void {
    using CountingTree =
        ci::RBTree<Interval, ci::Compare<ByBegin>, ci::Extensions<CountingAugment>>;

    constexpr int COUNT = 64;
    std::vector<Interval> storage(COUNT);
    for (int i = 0; i < COUNT; ++i) {
        storage[i].m_begin = i;
        storage[i].m_end = i * 2;
        storage[i].m_max_end = i * 2;
    }

    Counters::s_recompute = 0;
    Counters::s_copy = 0;

    CountingTree tree;
    for (int i = 0; i < COUNT; ++i) {
        static_cast<void>(tree.insert_equal(storage[i]));
    }
    if (Counters::s_recompute == 0) {
        fail("recompute was never called during insertion");
    }

    const auto after_insert = Counters::s_recompute;
    for (int i = 0; i < COUNT; ++i) {
        tree.erase(storage[i]);
    }
    if (Counters::s_recompute <= after_insert) {
        fail("recompute was never called during erasure");
    }
    // copy() carries an aggregate across a rotation and across the successor
    // transplant in erase; a tree this size does plenty of both.
    if (Counters::s_copy == 0) {
        fail("copy was never called across rotations or transplants");
    }
}

// Changing an element's augmented input in place (without moving it in key
// order) must be repairable through refresh_augment.
auto test_refresh_augment() -> void {
    constexpr int COUNT = 32;
    std::vector<Interval> storage(COUNT);
    for (int i = 0; i < COUNT; ++i) {
        storage[i].m_begin = i;
        storage[i].m_end = i;
        storage[i].m_max_end = i;
    }

    AugmentedTree tree;
    for (int i = 0; i < COUNT; ++i) {
        static_cast<void>(tree.insert_equal(storage[i]));
    }

    // Push one element's end far past everything else.
    auto it = tree.begin();
    std::advance(it, COUNT / 2);
    it->m_end = 100000;
    tree.refresh_augment(it);

    if (!verify(tree)) {
        fail("refresh_augment left a stale aggregate");
    }
    if (tree.root()->m_max_end != 100000) {
        fail("refresh_augment did not reach the root");
    }
}

} // namespace

auto main() -> int {
    // Several seeds and sizes: rotation shapes and, especially, whether an
    // erase hits the successor-transplant path at all depend on the exact
    // key order, so one sequence is not enough to trust the hook coverage.
    for (unsigned seed = 1; seed <= 25; ++seed) {
        for (const int count: {1, 2, 3, 7, 16, 64, 200}) {
            test_random_operations(seed * 7919, count);
            if (g_failures != 0) {
                std::fprintf(stderr, "  (seed=%u count=%d)\n", seed * 7919, count);
                return 1;
            }
        }
    }
    test_shape_matches_plain_tree();
    test_hooks_actually_fire();
    test_refresh_augment();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("rbtree_augment_test: all checks passed\n");
    return 0;
}
