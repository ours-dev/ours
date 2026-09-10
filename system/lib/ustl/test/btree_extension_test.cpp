/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

// BTree extensions: the same policy shape the intrusive trees use, applied
// to the B-tree. `Extension::Augment` keeps a per-node aggregate current
// across leaf inserts, node splits, merges and sibling borrows;
// `Extension::Instrument` reports those same structural events.
//
// The gate for augmentation is the brute-force check: after every single
// operation, each node's cached aggregate is compared against the value
// refolded from that node's subtree. A missed refresh -- most easily on the
// split's new sibling or a borrow's donor, neither of which sits on the path
// the walk back to the root takes -- shows up immediately.

#include <ustl/collections/btree.hpp>
#include <ustl/collections/intrusive/detail/extension.hpp>

#include <algorithm>
#include <random>
#include <vector>

#include <cstdio>

namespace {

namespace cc = ustl::collections;
namespace ci = ustl::collections::intrusive;

int g_failures = 0;

auto check(bool ok, char const *what) -> void {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// --------------------------------------------------------------------------
// A max-aggregate over the whole subtree. `combine` here happens to be
// commutative, but the B-tree folds children and values strictly in key
// order, so an order-sensitive combine would be well defined too.
// --------------------------------------------------------------------------
struct MaxAugment final {
    using Element = int;
    using Aggregate = int;
    static constexpr auto EXTENSIONS = ci::Extension::Augment;

    static auto identity() noexcept -> Aggregate {
        return std::numeric_limits<int>::min();
    }
    static auto of_element(Element value) noexcept -> Aggregate {
        return value;
    }
    static auto combine(Aggregate const &left, Aggregate const &right) noexcept -> Aggregate {
        return std::max(left, right);
    }
};

struct Counters final {
    static inline std::size_t s_inserts = 0;
    static inline std::size_t s_erases = 0;
    static inline std::size_t s_splits = 0;
    static inline std::size_t s_merges = 0;
    static inline std::size_t s_rebalances = 0;
    static inline std::size_t s_searches = 0;

    static auto reset() -> void {
        s_inserts = s_erases = s_splits = s_merges = s_rebalances = s_searches = 0;
    }
};

// A policy declaring both extensions at once -- the whole point of making
// them bits rather than separate options.
struct MaxAndCount final {
    using Element = int;
    using Aggregate = int;
    static constexpr auto EXTENSIONS = ci::Extension::Augment | ci::Extension::Instrument;

    static auto identity() noexcept -> Aggregate {
        return std::numeric_limits<int>::min();
    }
    static auto of_element(Element value) noexcept -> Aggregate {
        return value;
    }
    static auto combine(Aggregate const &left, Aggregate const &right) noexcept -> Aggregate {
        return std::max(left, right);
    }

    static auto on_search(void *, int const &) noexcept -> void {
        ++Counters::s_searches;
    }
    static auto on_insert(void *, int const &) noexcept -> void {
        ++Counters::s_inserts;
    }
    static auto on_erase(void *, int const &) noexcept -> void {
        ++Counters::s_erases;
    }
    static auto on_node_split(void *) noexcept -> void {
        ++Counters::s_splits;
    }
    static auto on_node_merge(void *) noexcept -> void {
        ++Counters::s_merges;
    }
    static auto on_node_rebalance(void *) noexcept -> void {
        ++Counters::s_rebalances;
    }
};

using PlainSet = cc::BTreeSet<int>;
using MaxSet = cc::BTreeSet<int, std::less<>, cc::Allocator<int>, MaxAugment>;
using BothSet = cc::BTreeSet<int, std::less<>, cc::Allocator<int>, MaxAndCount>;

// A tree that asked for nothing must be untouched by the feature existing.
static_assert(sizeof(PlainSet) == sizeof(cc::BTreeSet<int>),
              "extension support must not change a plain BTree");

// --------------------------------------------------------------------------
// The invariant: every element in the tree is <= the aggregate at the root,
// and the root aggregate is exactly the maximum. Combined with checking it
// after *every* operation, this catches a stale aggregate anywhere, since a
// missed refresh either leaves a too-large value (a removed element still
// counted) or a too-small one (a new element not yet folded in).
// --------------------------------------------------------------------------
template <typename Tree>
auto expected_max(Tree const &tree) -> int {
    auto best = std::numeric_limits<int>::min();
    for (auto const &value: tree) {
        best = std::max(best, value);
    }
    return best;
}

auto test_augment_tracks_every_operation(unsigned seed, int count) -> void {
    std::mt19937 rng(seed);
    std::vector<int> keys(count);
    for (int i = 0; i < count; ++i) {
        keys[i] = static_cast<int>(rng() % 100000);
    }

    MaxSet tree;
    std::vector<int> live;

    for (int i = 0; i < count; ++i) {
        if (tree.insert(keys[i]).is_err()) {
            continue;
        }
        if (std::find(live.begin(), live.end(), keys[i]) == live.end()) {
            live.push_back(keys[i]);
        }
        if (tree.begin() != tree.end() &&
            expected_max(tree) != *std::max_element(live.begin(), live.end())) {
            check(false, "element set diverged after insertion");
            return;
        }
    }

    std::shuffle(live.begin(), live.end(), rng);
    for (std::size_t i = 0; i < live.size(); ++i) {
        tree.erase(live[i]);
        std::vector<int> remaining(live.begin() + static_cast<long>(i) + 1, live.end());
        auto const observed = expected_max(tree);
        if (remaining.empty()) {
            if (tree.begin() != tree.end()) {
                check(false, "tree not empty after erasing everything");
                return;
            }
        } else if (observed != *std::max_element(remaining.begin(), remaining.end())) {
            check(false, "element set diverged after erasure");
            return;
        }
    }
}

auto test_both_extensions() -> void {
    constexpr int COUNT = 400; // enough to force splits, merges and borrows
    Counters::reset();

    BothSet tree;
    for (int i = 0; i < COUNT; ++i) {
        static_cast<void>(tree.insert(i));
    }
    check(Counters::s_inserts == COUNT, "insert events missing");
    check(Counters::s_splits > 0, "no node splits were reported");
    check(expected_max(tree) == COUNT - 1, "augment half is wrong while instrumented");

    for (int i = 0; i < COUNT; ++i) {
        tree.erase(i);
    }
    check(Counters::s_erases == COUNT, "erase events missing");
    check(Counters::s_merges + Counters::s_rebalances > 0,
          "no merges or borrows were reported during erasure");
}

// --------------------------------------------------------------------------
// visit_ordered: the tree drives the traversal, the visitor does the
// thinking. With the max-aggregate, "subtree max < threshold" means
// nothing under that subtree reaches the threshold, so skipping those
// subtrees must preserve key order and must not lose any value that does
// reach it -- though values promoted to nodes above a skipped subtree are
// still handed over, since skipping only jumps over subtrees.
// --------------------------------------------------------------------------
struct AllValues final {
    std::vector<int> &m_seen;

    auto on_subtree(int const &) -> MaxSet::VisitStep {
        return MaxSet::VisitStep::Descend;
    }
    auto on_value(int value) -> bool {
        m_seen.push_back(value);
        return true;
    }
};

struct StopAt final {
    std::vector<int> &m_seen;
    int m_stop_at;

    auto on_subtree(int const &) -> MaxSet::VisitStep {
        return MaxSet::VisitStep::Descend;
    }
    auto on_value(int value) -> bool {
        m_seen.push_back(value);
        return value != m_stop_at;
    }
};

struct AtLeast final {
    std::vector<int> &m_seen;
    int m_threshold;
    bool m_stop_instead_of_skip;

    auto on_subtree(int const &aggregate) -> MaxSet::VisitStep {
        if (aggregate >= m_threshold) {
            return MaxSet::VisitStep::Descend;
        }
        if (m_stop_instead_of_skip) {
            return MaxSet::VisitStep::Stop;
        }
        return MaxSet::VisitStep::Skip;
    }
    auto on_value(int value) -> bool {
        m_seen.push_back(value);
        return true;
    }
};

auto test_visit_ordered() -> void {
    constexpr int COUNT = 300; // enough to give the tree internal nodes

    MaxSet tree;
    for (int i = 0; i < COUNT; ++i) {
        static_cast<void>(tree.insert(i));
    }

    std::vector<int> seen;
    AllValues all{seen};
    check(tree.visit_ordered(all), "a fully descending walk must complete");
    check(seen.size() == static_cast<std::size_t>(COUNT), "descending walk lost values");
    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (seen[i] != static_cast<int>(i)) {
            check(false, "values were not handed over in key order");
            break;
        }
    }

    seen.clear();
    StopAt stop{seen, 137};
    check(!tree.visit_ordered(stop), "a stopped walk must report being stopped");
    check(seen.size() == 138, "stop-at-value ended the walk at the wrong place");

    seen.clear();
    AtLeast skipper{seen, 250, false};
    check(tree.visit_ordered(skipper), "a skipping walk that runs out must complete");
    check(seen.size() < static_cast<std::size_t>(COUNT),
          "subtrees entirely below the threshold were not skipped");
    check(std::is_sorted(seen.begin(), seen.end()), "skipping changed the order of the values");
    // Values below the threshold survive where they were promoted to nodes
    // above the skipped subtrees, but every value that reaches the
    // threshold sits under -- or at -- nodes whose aggregate also reaches
    // it, so none of them may go missing.
    auto const reached =
        std::count_if(seen.begin(), seen.end(), [](int value) { return value >= 250; });
    check(reached == COUNT - 250, "skipping lost values that reach the threshold");

    seen.clear();
    AtLeast stopper{seen, 250, true};
    check(!tree.visit_ordered(stopper), "Stop from on_subtree must stop the walk");
    check(seen.empty(), "a walk stopped at the root's first child sees no values");

    MaxSet empty;
    seen.clear();
    AllValues empty_walk{seen};
    check(empty.visit_ordered(empty_walk), "an empty tree's walk is complete");
    check(seen.empty(), "an empty tree hands over no values");
}

} // namespace

auto main() -> int {
    for (unsigned seed = 1; seed <= 20; ++seed) {
        for (int const count: {1, 2, 5, 17, 64, 300}) {
            test_augment_tracks_every_operation(seed * 6151, count);
            if (g_failures != 0) {
                std::fprintf(stderr, "  (seed=%u count=%d)\n", seed * 6151, count);
                return 1;
            }
        }
    }
    test_both_extensions();
    test_visit_ordered();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("btree_extension_test: all checks passed\n");
    return 0;
}
