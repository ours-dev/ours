/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/slist.hpp>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <random>
#include <utility>
#include <vector>

#include <cstddef>
#include <cstdio>
#include <cstdlib>

// With CacheLast, Slist keeps its last node in a member rather than finding it
// by walking, so every operation that changes a chain has to keep that member
// right, each in its own way. This drives long random sequences of those
// operations over two lists next to a plain model of what each should hold.
// After every step each list must match its model, and a push_back() -- which
// links after the cached last node -- must land at the very end: a stale cache
// shows up as an element linked into the middle of a list, or into the other
// list. Lists without CacheLast run the same sequences as a control, and both
// circular and linear lists run, since their end nodes differ.

namespace {
namespace intrusive = ustl::collections::intrusive;

struct Value final: intrusive::SlistBaseHook<> {
    int m_key{};
};

using CachedSlist = intrusive::Slist<Value, intrusive::CacheLast<true>>;
using CachedLinearSlist =
    intrusive::Slist<Value, intrusive::Linear<true>, intrusive::CacheLast<true>>;
using PlainSlist = intrusive::Slist<Value>;
using PlainLinearSlist = intrusive::Slist<Value, intrusive::Linear<true>>;

using Model = std::vector<int>;

constexpr auto POOL_SIZE = std::size_t{12};
constexpr auto STEPS = 20000;
constexpr auto OPERATIONS = std::size_t{15};

template <typename List>
auto matches(List const &list, Model const &model) -> bool {
    auto index = std::size_t{};
    for (auto const &value: list) {
        if (index == model.size() || value.m_key != model[index]) {
            return false;
        }
        ++index;
    }
    return index == model.size() && list.size() == model.size();
}

/// The iterator `index` steps past before_begin(): 0 is before_begin() itself
/// and k is the k-th element, so inserting after it puts a value at model
/// index `index`.
template <typename List>
auto at(List &list, std::size_t const index) -> typename List::IterMut {
    auto position = list.before_begin();
    for (auto step = std::size_t{}; step < index; ++step) {
        ++position;
    }
    return position;
}

/// push_back() links after the cached last node, so `probe` must come out
/// last. Erasing it again must leave the cache on the node before it, which
/// the next step's probe checks in turn.
template <typename List>
auto cache_holds(List &list, Model const &model, Value &probe) -> bool {
    list.push_back(probe);
    auto expected = model;
    expected.push_back(probe.m_key);
    if (!matches(list, expected)) {
        return false;
    }
    auto _ = list.erase(list.iterator_to(probe));
    return matches(list, model);
}

template <typename List>
auto run(char const *const name, unsigned const seed) -> void {
    std::array<Value, POOL_SIZE> pool{};
    for (auto key = std::size_t{}; key < POOL_SIZE; ++key) {
        pool[key].m_key = static_cast<int>(key);
    }
    Value probe{};
    probe.m_key = static_cast<int>(POOL_SIZE);
    std::array<List, 2> lists{};
    std::array<Model, 2> models{};
    std::minstd_rand random(seed);

    const auto below = [&random](std::size_t const bound) -> std::size_t {
        return static_cast<std::size_t>(random()) % bound;
    };
    // A value in neither list, or none when all of them are in use.
    const auto free_key = [&models, &below]() -> std::size_t {
        std::vector<std::size_t> candidates{};
        for (auto key = std::size_t{}; key < POOL_SIZE; ++key) {
            const auto holds_key = [key](Model const &model) {
                return std::ranges::find(model, static_cast<int>(key)) != model.end();
            };
            if (!holds_key(models[0]) && !holds_key(models[1])) {
                candidates.push_back(key);
            }
        }
        if (candidates.empty()) {
            return POOL_SIZE;
        }
        return candidates[below(candidates.size())];
    };
    const auto less = [](Value const &left, Value const &right) noexcept {
        return left.m_key < right.m_key;
    };

    for (auto step = 0; step < STEPS; ++step) {
        const auto side = below(2);
        auto &list = lists[side];
        auto &model = models[side];
        auto &other = lists[1 - side];
        auto &other_model = models[1 - side];
        const auto operation = below(OPERATIONS);
        switch (operation) {
            case 0: {
                const auto key = free_key();
                if (key != POOL_SIZE) {
                    const auto index = below(model.size() + 1);
                    auto _ = list.insert_after(at(list, index), pool[key]);
                    model.insert(model.begin() + static_cast<std::ptrdiff_t>(index),
                                 static_cast<int>(key));
                }
                break;
            }
            case 1: {
                const auto key = free_key();
                if (key != POOL_SIZE) {
                    list.push_back(pool[key]);
                    model.push_back(static_cast<int>(key));
                }
                break;
            }
            case 2: {
                // insert(end(), ...) finds the node before end() through the cache.
                const auto key = free_key();
                if (key != POOL_SIZE) {
                    auto _ = list.insert(list.end(), pool[key]);
                    model.push_back(static_cast<int>(key));
                }
                break;
            }
            case 3: {
                const auto key = free_key();
                if (key != POOL_SIZE) {
                    list.push_front(pool[key]);
                    model.insert(model.begin(), static_cast<int>(key));
                }
                break;
            }
            case 4: {
                if (!model.empty()) {
                    const auto index = below(model.size());
                    auto _ = list.erase_after(at(list, index));
                    model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
                }
                break;
            }
            case 5: {
                if (!model.empty()) {
                    const auto index = below(model.size());
                    const auto key = static_cast<std::size_t>(model[index]);
                    auto _ = list.erase(list.iterator_to(pool[key]));
                    model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
                }
                break;
            }
            case 6: {
                const auto index = below(model.size() + 1);
                list.splice_after(at(list, index), other);
                model.insert(model.begin() + static_cast<std::ptrdiff_t>(index),
                             other_model.begin(), other_model.end());
                other_model.clear();
                break;
            }
            case 7: {
                // A range of either list, moved after a position in `list`. When
                // both are the same list the position must lie outside the range,
                // and a position right before the range is a valid no-op.
                const auto from_side = below(2);
                auto &source = lists[from_side];
                auto &source_model = models[from_side];
                if (source_model.empty()) {
                    break;
                }
                const auto first = below(source_model.size());
                const auto count = 1 + below(source_model.size() - first);
                const Model moved(source_model.begin() + static_cast<std::ptrdiff_t>(first),
                                  source_model.begin() +
                                      static_cast<std::ptrdiff_t>(first + count));
                auto position = std::size_t{};
                auto insert_at = std::size_t{};
                if (&source == &list) {
                    position = below(model.size() - count + 1);
                    if (position > first) {
                        position += count;
                        insert_at = position - count;
                    } else {
                        insert_at = position;
                    }
                } else {
                    position = below(model.size() + 1);
                    insert_at = position;
                }
                list.splice_after(at(list, position), source, at(source, first),
                                  at(source, first + count), count);
                source_model.erase(source_model.begin() + static_cast<std::ptrdiff_t>(first),
                                   source_model.begin() +
                                       static_cast<std::ptrdiff_t>(first + count));
                model.insert(model.begin() + static_cast<std::ptrdiff_t>(insert_at), moved.begin(),
                             moved.end());
                break;
            }
            case 8: {
                // splice(end(), ...) also finds the node before end() through the
                // cache.
                if (!other_model.empty()) {
                    const auto index = below(other_model.size());
                    const auto key = static_cast<std::size_t>(other_model[index]);
                    list.splice(list.end(), other, other.iterator_to(pool[key]));
                    other_model.erase(other_model.begin() + static_cast<std::ptrdiff_t>(index));
                    model.push_back(static_cast<int>(key));
                }
                break;
            }
            case 9: {
                list.swap(other);
                std::swap(model, other_model);
                break;
            }
            case 10: {
                list.reverse();
                std::ranges::reverse(model);
                break;
            }
            case 11: {
                list.sort(less);
                std::ranges::sort(model);
                break;
            }
            case 12: {
                list.sort(less);
                other.sort(less);
                list.merge(other, less);
                // Keys are unique, so merging two sorted lists gives the sorted union.
                model.insert(model.end(), other_model.begin(), other_model.end());
                std::ranges::sort(model);
                other_model.clear();
                break;
            }
            case 13: {
                const auto odd = [](int const key) { return key % 2 != 0; };
                auto _ = list.remove_if(
                    [&odd](Value const &value) noexcept { return odd(value.m_key); });
                std::erase_if(model, odd);
                break;
            }
            default: {
                // A move out and back: move construction swaps with a new, empty
                // list, and move assignment clears and then swaps.
                List moved(std::move(list));
                list = std::move(moved);
                break;
            }
        }

        if (!matches(lists[0], models[0]) || !matches(lists[1], models[1]) ||
            !cache_holds(lists[0], models[0], probe) || !cache_holds(lists[1], models[1], probe)) {
            std::fprintf(stderr, "%s: seed %u, step %d, operation %zu: list and model differ\n",
                         name, seed, step, operation);
            // The lists may be linked into each other; tearing them down could
            // spin, so leave without running destructors.
            std::_Exit(1);
        }
    }
    lists[0].clear();
    lists[1].clear();
}
} // namespace

auto main() -> int {
    for (auto const seed: {1U, 2U, 3U}) {
        run<CachedSlist>("circular+cache_last", seed);
        run<CachedLinearSlist>("linear+cache_last", seed);
        run<PlainSlist>("circular", seed);
        run<PlainLinearSlist>("linear", seed);
    }
    return 0;
}
