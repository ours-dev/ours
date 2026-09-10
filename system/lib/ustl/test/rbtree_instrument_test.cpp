/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

// Tree instrumentation: an observer that reports insertion depth, rotations
// and rebalancing steps, plumbed exactly the way augmentation is -- a table
// of function pointers built from a policy that only ever sees the element
// type.
//
// Two properties matter here beyond "the counters move":
//
//  * an uninstrumented tree is unchanged -- same size, and every hook site
//    folds away at compile time, so a shipping build pays nothing;
//  * the observer state is per tree, not global, so two trees can be
//    measured independently. That is the piece a static template observer
//    cannot express.

#include <vector>

#include <cstdio>

#include <ustl/collections/intrusive/rbtree.hpp>

namespace {

struct Value final: ustl::collections::intrusive::SetBaseHook<> {
    int m_key{};
};

struct ByKey final {
    [[nodiscard]]
    constexpr auto operator()(Value const &left, Value const &right) const noexcept -> bool {
        return left.m_key < right.m_key;
    }
};

// What an observer accumulates. One of these per tree.
struct Stats final {
    std::size_t inserts = 0;
    std::size_t erases = 0;
    std::size_t rotations = 0;
    std::size_t rebalance_steps = 0;
    std::size_t total_depth = 0;
    std::size_t max_depth = 0;
};

// The policy: only ever sees `Value`, never a tree node.
struct CountingInstrument final {
    using Element = Value;
    static constexpr auto EXTENSIONS = ustl::collections::intrusive::Extension::Instrument;

    static auto on_insert(void *context, Element &, std::size_t depth) noexcept -> void {
        auto &stats = *static_cast<Stats *>(context);
        ++stats.inserts;
        stats.total_depth += depth;
        if (depth > stats.max_depth) {
            stats.max_depth = depth;
        }
    }
    static auto on_erase(void *context, Element &) noexcept -> void {
        ++static_cast<Stats *>(context)->erases;
    }
    static auto on_rotate(void *context, Element &, Element &) noexcept -> void {
        ++static_cast<Stats *>(context)->rotations;
    }
    static auto on_rebalance_step(void *context, bool) noexcept -> void {
        ++static_cast<Stats *>(context)->rebalance_steps;
    }
};

namespace ci = ustl::collections::intrusive;

using PlainTree = ci::RBTree<Value, ci::Compare<ByKey>>;
using WatchedTree = ci::RBTree<Value, ci::Compare<ByKey>, ci::Extensions<CountingInstrument>>;

// An uninstrumented tree must be untouched by the feature existing.
static_assert(sizeof(PlainTree) == sizeof(ci::RBTree<Value, ci::Compare<ByKey>>),
              "instrumentation support must not change a plain tree");
// The per-tree context is one pointer, and only on an instrumented tree.
static_assert(sizeof(WatchedTree) == sizeof(PlainTree) + sizeof(void *),
              "an instrumented tree should cost exactly one context pointer");

int g_failures = 0;

auto check(bool ok, char const *what) -> void {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

auto test_events_fire() -> void {
    constexpr int COUNT = 256;
    std::vector<Value> storage(COUNT);
    for (int i = 0; i < COUNT; ++i) {
        storage[i].m_key = i; // ascending: the worst case for rebalancing
    }

    Stats stats;
    WatchedTree tree;
    tree.set_instrument_context(&stats);

    for (int i = 0; i < COUNT; ++i) {
        static_cast<void>(tree.insert_equal(storage[i]));
    }
    check(stats.inserts == COUNT, "one insert event per insertion");
    check(stats.rotations > 0, "ascending inserts should have forced rotations");
    check(stats.max_depth > 0, "insertion depth should have been reported");

    // A red-black tree stays within 2*log2(n+1); with n = 256 that is 16.
    check(stats.max_depth <= 16, "insertion depth exceeded the red-black bound");

    // Rebalancing is amortized constant: total restructuring work across n
    // insertions must stay O(n), not O(n log n).
    const auto work = stats.rotations + stats.rebalance_steps;
    check(work < static_cast<std::size_t>(COUNT) * 3,
          "rebalancing work per insertion is not amortized constant");

    for (int i = 0; i < COUNT; ++i) {
        tree.erase(storage[i]);
    }
    check(stats.erases == COUNT, "one erase event per erasure");
}

// The point of a per-tree context: two trees, two independent tallies.
auto test_context_is_per_tree() -> void {
    constexpr int COUNT = 32;
    std::vector<Value> a_storage(COUNT);
    std::vector<Value> b_storage(COUNT);
    for (int i = 0; i < COUNT; ++i) {
        a_storage[i].m_key = i;
        b_storage[i].m_key = i;
    }

    Stats a_stats;
    Stats b_stats;
    WatchedTree a;
    WatchedTree b;
    a.set_instrument_context(&a_stats);
    b.set_instrument_context(&b_stats);

    for (int i = 0; i < COUNT; ++i) {
        static_cast<void>(a.insert_equal(a_storage[i]));
    }
    for (int i = 0; i < COUNT / 2; ++i) {
        static_cast<void>(b.insert_equal(b_storage[i]));
    }

    check(a_stats.inserts == COUNT, "tree A's tally is wrong");
    check(b_stats.inserts == COUNT / 2, "tree B's tally is wrong");
    check(a_stats.inserts != b_stats.inserts, "the two trees shared one tally");

    a.clear();
    b.clear();
}

// Instrumentation must not perturb the tree it is measuring.
auto test_shape_is_unaffected() -> void {
    constexpr int COUNT = 128;
    std::vector<Value> plain_storage(COUNT);
    std::vector<Value> watched_storage(COUNT);
    for (int i = 0; i < COUNT; ++i) {
        plain_storage[i].m_key = (i * 37) % COUNT;
        watched_storage[i].m_key = (i * 37) % COUNT;
    }

    Stats stats;
    PlainTree plain;
    WatchedTree watched;
    watched.set_instrument_context(&stats);
    for (int i = 0; i < COUNT; ++i) {
        static_cast<void>(plain.insert_equal(plain_storage[i]));
        static_cast<void>(watched.insert_equal(watched_storage[i]));
    }

    auto plain_it = plain.begin();
    auto watched_it = watched.begin();
    for (; plain_it != plain.end() && watched_it != watched.end(); ++plain_it, ++watched_it) {
        if (plain_it->m_key != watched_it->m_key) {
            check(false, "instrumented tree ordered differently");
            break;
        }
    }
    plain.clear();
    watched.clear();
}

// ---------------------------------------------------------------------------
// The point of making these extensions rather than separate options: one
// policy can declare both, and the tree activates both from the same bit set
// without the container growing another branch or the algorithms another
// overload.
// ---------------------------------------------------------------------------
struct Augmented final: ustl::collections::intrusive::SetBaseHook<> {
    int m_key{};
    int m_max_key{}; // the augmented aggregate
};

struct ByAugmentedKey final {
    [[nodiscard]]
    constexpr auto operator()(Augmented const &l, Augmented const &r) const noexcept -> bool {
        return l.m_key < r.m_key;
    }
};

struct BothExtensions final {
    using Element = Augmented;
    static constexpr auto EXTENSIONS = ci::Extension::Augment | ci::Extension::Instrument;

    // Extension::Augment
    static auto recompute(Element &self, Element const *left, Element const *right) noexcept
        -> bool {
        auto value = self.m_key;
        if (left != nullptr && left->m_max_key > value) {
            value = left->m_max_key;
        }
        if (right != nullptr && right->m_max_key > value) {
            value = right->m_max_key;
        }
        const auto unchanged = value == self.m_max_key;
        self.m_max_key = value;
        return unchanged;
    }
    static auto copy(Element const &src, Element &dst) noexcept -> void {
        dst.m_max_key = src.m_max_key;
    }

    // Extension::Instrument
    static auto on_insert(void *ctx, Element &, std::size_t) noexcept -> void {
        ++static_cast<Stats *>(ctx)->inserts;
    }
    static auto on_erase(void *ctx, Element &) noexcept -> void {
        ++static_cast<Stats *>(ctx)->erases;
    }
    static auto on_rotate(void *ctx, Element &, Element &) noexcept -> void {
        ++static_cast<Stats *>(ctx)->rotations;
    }
    static auto on_rebalance_step(void *ctx, bool) noexcept -> void {
        ++static_cast<Stats *>(ctx)->rebalance_steps;
    }
};

auto test_both_extensions_at_once() -> void {
    using BothTree =
        ci::RBTree<Augmented, ci::Compare<ByAugmentedKey>, ci::Extensions<BothExtensions>>;
    constexpr int COUNT = 128;
    std::vector<Augmented> storage(COUNT);
    for (int i = 0; i < COUNT; ++i) {
        storage[i].m_key = (i * 53) % COUNT;
        storage[i].m_max_key = storage[i].m_key;
    }

    Stats stats;
    BothTree tree;
    tree.set_instrument_context(&stats);
    for (int i = 0; i < COUNT; ++i) {
        static_cast<void>(tree.insert_equal(storage[i]));
    }

    // The instrumentation half fired ...
    check(stats.inserts == COUNT, "combined policy: insert events missing");
    check(stats.rotations > 0, "combined policy: rotation events missing");
    // ... and the augmentation half is exact at the same time.
    check(tree.root()->m_max_key == COUNT - 1, "combined policy: aggregate is wrong");

    for (int i = 0; i < COUNT; ++i) {
        tree.erase(storage[i]);
    }
    check(stats.erases == COUNT, "combined policy: erase events missing");
}

} // namespace

auto main() -> int {
    test_events_fire();
    test_context_is_per_tree();
    test_shape_is_unaffected();
    test_both_extensions_at_once();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("rbtree_instrument_test: all checks passed\n");
    return 0;
}
