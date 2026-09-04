/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Host benchmark for the tree access patterns VmaSet's walks rest on.
///
/// Lives under //benches rather than //kernel/mm/tests because it reports
/// timings instead of passing or failing; the tree here mirrors the source
/// tree, so it sits at the path of the code it measures.
///
/// It exists because the traversal changes in docs/changes/260903/mm.md
/// sections 2 and 3 were argued from complexity alone, which is not evidence.
/// What it measures:
///
///   A. point lookup   -- the VmArea::find_mapping_locked() shape, on the page
///                        fault path: scanning from begin() versus seeking.
///   B. range walk     -- the VmArea::protect_locked() shape.
///   C. per-step cost  -- re-seeking every step versus ++, which is the whole
///                        source of section 3's gain.
///   D. erase pass     -- re-seeking per erase versus erase(iterator), which is
///                        where section 3 claims *no* gain, because ustl's
///                        erase(iterator) is itself a re-seek.
///
/// Deliberately not built on VmaSet: that would drag in VmAreaOrMapping, the
/// address space and the canary, none of which the numbers are about. The
/// container, key type and key extractor are the same shape, so what is being
/// measured is the tree. The real VmaSet also carries GapAugment, which this
/// omits -- augmentation costs insert and erase a little more, and does not
/// touch the walks at all.
///
/// Host numbers, host compiler, host cache. They say how the curves behave as
/// the child count grows, not how many nanoseconds the kernel will spend.
#include <ustl/collections/allocator.hpp>
#include <ustl/collections/detail/btree.hpp>

#include <chrono>
#include <functional>
#include <random>
#include <vector>

#include <cstdint>
#include <cstdio>

namespace {
using u64 = std::uint64_t;
constexpr u64 PAGE = 4096;

struct Area {
    u64 base;
    u64 size;

    auto end() const -> u64 {
        return base + size;
    }
};

struct BaseOf final {
    auto operator()(Area *area) const noexcept -> u64 {
        return area->base;
    }
};

using Tree = ustl::collections::detail::BTree<u64, Area *, BaseOf, std::less<>,
                                              ustl::collections::Allocator<Area *>>;

/// Two-page areas with a one-page gap between them, so a window can both hit
/// an area and fall in a hole.
auto build(std::vector<Area> &store, Tree &tree, std::size_t n) -> void {
    store.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        store[i] = Area{.base = u64(i) * 3 * PAGE, .size = 2 * PAGE};
    }
    for (auto &area: store) {
        (void)tree.insert(&area);
    }
}

/// What VmaSet::seek() does: the lower bound, or the area before it when that
/// one starts lower and still reaches past |addr|.
auto seek(Tree &tree, u64 addr) -> Tree::iterator {
    auto const at_or_after = tree.lower_bound(addr);
    auto iter = at_or_after.is_some() ? at_or_after.value() : tree.end();
    if (iter != tree.begin()) {
        auto previous = iter;
        --previous;
        if ((*previous)->end() > addr) {
            return previous;
        }
    }
    return iter;
}

template <typename F>
auto time_ns(std::size_t reps, F &&fn) -> double {
    auto const started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < reps; ++i) {
        fn(i);
    }
    auto const ended = std::chrono::steady_clock::now();
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count()) /
           double(reps);
}

std::size_t SINK = 0;

auto report(std::size_t n, char const *what, double old_ns, double now_ns) -> void {
    std::printf("%6zu | %-28s | %9.1f  %9.1f  %6.2fx\n", n, what, old_ns, now_ns, old_ns / now_ns);
}
} // namespace

auto main() -> int {
    std::printf("%6s | %-28s | %9s  %9s  %7s\n", "n", "operation", "before", "after", "speedup");
    std::printf("-------+------------------------------+--------------------------------\n");

    for (std::size_t n: {8u, 64u, 512u, 4096u}) {
        std::vector<Area> store;
        Tree tree;
        build(store, tree, n);

        std::mt19937_64 rng{12345};
        std::vector<u64> targets(4096);
        for (auto &target: targets) {
            target = (rng() % n) * 3 * PAGE;
        }

        // A. point lookup, the page-fault path.
        auto const scan_point = time_ns(targets.size(), [&](std::size_t i) {
            u64 const va = targets[i];
            for (auto *area: tree) {
                if (area->base > va) {
                    break;
                }
                if (va - area->base < area->size) {
                    SINK += 1;
                    break;
                }
            }
        });
        auto const seek_point = time_ns(targets.size(), [&](std::size_t i) {
            u64 const va = targets[i];
            auto const iter = seek(tree, va);
            if (iter != tree.end() && va - (*iter)->base < (*iter)->size) {
                SINK += 1;
            }
        });
        report(n, "point lookup (fault path)", scan_point, seek_point);

        // B. range walk over eight areas.
        constexpr std::size_t SPAN = 8;
        auto const scan_range = time_ns(targets.size(), [&](std::size_t i) {
            u64 const low = targets[i];
            u64 const high = low + SPAN * 3 * PAGE;
            for (auto *area: tree) {
                if (area->base >= high) {
                    break;
                }
                if (area->end() <= low) {
                    continue;
                }
                SINK += 1;
            }
        });
        auto const seek_range = time_ns(targets.size(), [&](std::size_t i) {
            u64 const low = targets[i];
            u64 const high = low + SPAN * 3 * PAGE;
            for (auto iter = seek(tree, low); iter != tree.end() && (*iter)->base < high; ++iter) {
                SINK += 1;
            }
        });
        report(n, "range walk (8 areas)", scan_range, seek_range);

        // C. per-step: re-seek every step versus ++.
        auto const step_seek = time_ns(targets.size(), [&](std::size_t i) {
            u64 cursor = targets[i];
            for (std::size_t k = 0; k < SPAN; ++k) {
                auto const iter = seek(tree, cursor);
                if (iter == tree.end()) {
                    break;
                }
                cursor = (*iter)->end();
                SINK += 1;
            }
        });
        auto const step_incr = time_ns(targets.size(), [&](std::size_t i) {
            auto iter = seek(tree, targets[i]);
            for (std::size_t k = 0; k < SPAN && iter != tree.end(); ++k, ++iter) {
                SINK += 1;
            }
        });
        report(n, "8 steps: re-seek vs ++", step_seek, step_incr);

        // D. erasing every area, which is what an unmap that covers everything
        // does. Both ways pay a descent per erase, so this is the measurement
        // that has to show no gain if the claim about erase(iterator) is right.
        double erase_seek = 0;
        double erase_iter = 0;
        {
            std::vector<Area> scratch;
            Tree victim;
            build(scratch, victim, n);
            erase_seek = time_ns(1,
                                 [&](std::size_t) {
                                     u64 cursor = 0;
                                     while (true) {
                                         auto const iter = seek(victim, cursor);
                                         if (iter == victim.end()) {
                                             break;
                                         }
                                         cursor = (*iter)->end();
                                         victim.erase((*iter)->base);
                                     }
                                 }) /
                         double(n);
        }
        {
            std::vector<Area> scratch;
            Tree victim;
            build(scratch, victim, n);
            erase_iter = time_ns(1,
                                 [&](std::size_t) {
                                     auto iter = seek(victim, 0);
                                     while (iter != victim.end()) {
                                         iter = victim.erase(iter);
                                     }
                                 }) /
                         double(n);
        }
        report(n, "erase all (per area)", erase_seek, erase_iter);

        std::printf("%6s | %-28s | %s\n", "", "", "");
    }

    return SINK != 0 ? 0 : 1;
}
