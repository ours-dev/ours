/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/error.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/result.hpp>

#include <concepts>

namespace {
struct Value final: ustl::collections::intrusive::ListBaseHook<> {
    int m_key{};
};

using List = ustl::collections::intrusive::List<Value>;

static_assert(std::same_as<List::Element, Value>);
static_assert(std::same_as<List::IterMut::PtrMut, Value *>);
static_assert(std::same_as<List::Iter::Ptr, Value const *>);
} // namespace

auto main() -> int {
    Value one{.m_key = 1};
    Value two{.m_key = 2};
    Value three{.m_key = 3};
    Value four{.m_key = 4};
    List list{};

    const auto first = list.insert(list.end(), one);
    static_cast<void>(list.insert(list.end(), two));
    static_cast<void>(list.insert(list.end(), three));
    const auto inserted = list.insert(first, four);
    if (inserted->m_key != 4 || list.front().value().m_key != 4 || list.back().value().m_key != 3) {
        return 1;
    }

    List const &const_list = list;
    if (const_list.cbegin()->m_key != 4 || const_list.crbegin()->m_key != 3 ||
        list.rbegin()->m_key != 3) {
        return 2;
    }

    const auto after_one = list.erase(list.iterator_to(one));
    if (after_one->m_key != 2 || one.is_linked() || list.size() != 3U) {
        return 3;
    }

    const auto after_range = list.erase(++list.begin(), list.end());
    if (after_range != list.end() || list.size() != 1U || list.begin()->m_key != 4 ||
        two.is_linked() || three.is_linked()) {
        return 4;
    }

    auto disposed = 0U;
    list.clear_and_dispose([&disposed](Value *const value) noexcept {
        disposed += static_cast<unsigned>(value->m_key == 4);
    });
    if (!list.empty() || four.is_linked() || disposed != 1U) {
        return 5;
    }

    Value five{.m_key = 5};
    Value six{.m_key = 6};
    Value seven{.m_key = 7};
    Value eight{.m_key = 8};
    List left{};
    List right{};
    left.push_back(one);
    left.push_back(three);
    left.push_back(five);
    right.push_back(two);
    right.push_back(four);
    right.push_back(six);
    left.splice(left.iterator_to(five), right, right.iterator_to(two));
    left.splice(left.end(), right);
    left.sort([](Value const &left_value, Value const &right_value) noexcept {
        return left_value.m_key < right_value.m_key;
    });
    right.push_back(seven);
    right.push_back(eight);
    left.merge(right, [](Value const &left_value, Value const &right_value) noexcept {
        return left_value.m_key < right_value.m_key;
    });
    left.reverse();
    const auto removed =
        left.remove_if([](Value const &value) noexcept { return value.m_key % 2 == 0; });
    if (removed != 4U || !right.empty() || two.is_linked() || four.is_linked() || six.is_linked() ||
        eight.is_linked() || left.size() != 4U || left.begin()->m_key != 7) {
        return 6;
    }

    Value duplicate_one{.m_key = 1};
    Value duplicate_two{.m_key = 1};
    Value duplicate_three{.m_key = 2};
    List duplicates{};
    duplicates.push_back(duplicate_one);
    duplicates.push_back(duplicate_two);
    duplicates.push_back(duplicate_three);
    if (duplicates.unique([](Value const &left_value, Value const &right_value) noexcept {
            return left_value.m_key == right_value.m_key;
        }) != 1U ||
        duplicate_two.is_linked() || duplicates.size() != 2U) {
        return 7;
    }

    Value clone_one{};
    Value clone_two{};
    Value clone_three{};
    Value clone_four{};
    Value failed_clone{};
    auto clone_index = 0U;
    auto disposed_clones = 0U;
    Value *const clones[]{&clone_one, &clone_two, &clone_three, &clone_four};
    List copy{};
    const auto copied = copy.clone_from(
        left,
        [&clones, &clone_index](
            Value const &source) noexcept -> ustl::Result<Value *, ustl::collections::Error> {
            auto *const clone = clones[clone_index++];
            clone->m_key = source.m_key;
            return ustl::ok(clone);
        },
        [&disposed_clones](Value *) noexcept { ++disposed_clones; });
    if (copied.is_err() || copy.size() != 4U || copy.begin()->m_key != 7 || disposed_clones != 0U) {
        return 8;
    }

    List failed_copy{};
    const auto failed = failed_copy.clone_from(
        left,
        [&failed_clone](Value const &) noexcept -> ustl::Result<Value *, ustl::collections::Error> {
            if (failed_clone.m_key == 0) {
                failed_clone.m_key = 1;
                return ustl::ok(&failed_clone);
            }
            return ustl::err(ustl::collections::Error::OutOfMemory);
        },
        [&disposed_clones](Value *) noexcept { ++disposed_clones; });
    return failed.is_err() && failed_copy.empty() && !failed_clone.is_linked() &&
                   disposed_clones == 1U
               ? 0
               : 9;
}
