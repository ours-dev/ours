/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/error.hpp>
#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/result.hpp>

#include <concepts>

namespace {
struct Value final: ustl::collections::intrusive::SlistBaseHook<> {
    int m_key{};
};

using Slist = ustl::collections::intrusive::Slist<Value>;

static_assert(std::same_as<Slist::Element, Value>);
static_assert(std::same_as<Slist::IterMut::RefMut, Value &>);
static_assert(std::same_as<Slist::Iter::Ref, Value const &>);
} // namespace

auto main() -> int {
    Value one{.m_key = 1};
    Value two{.m_key = 2};
    Value three{.m_key = 3};
    Value four{.m_key = 4};
    Slist list{};

    const auto before = list.before_begin();
    const auto first = list.insert_after(before, one);
    const auto second = list.insert_after(first, two);
    static_cast<void>(list.insert(list.end(), three));
    if (++list.before_begin() != list.begin() || list.previous(list.iterator_to(three)) != second ||
        list.front().value().m_key != 1 || list.size() != 3U) {
        return 1;
    }
    const auto next = list.erase_after(before);
    if (next->m_key != 2 || one.is_linked() || list.size() != 2U) {
        return 2;
    }
    static_cast<void>(list.insert_after(before, four));
    static_cast<void>(list.erase(list.iterator_to(three)));
    Slist source{};
    source.push_front(one);
    source.push_front(three);
    list.splice_after(before, source, source.before_begin(), source.previous(source.end()));
    if (!source.empty() || list.size() != 4U || list.begin()->m_key != 3 || !one.is_linked() ||
        !three.is_linked()) {
        return 3;
    }
    auto disposed = 0U;
    list.clear_and_dispose([&disposed](Value *const value) noexcept {
        disposed += static_cast<unsigned>(value->m_key > 0);
    });
    if (!list.empty() || one.is_linked() || two.is_linked() || three.is_linked() ||
        four.is_linked() || disposed != 4U) {
        return 4;
    }
    Value five{.m_key = 5};
    Value six{.m_key = 6};
    Value seven{.m_key = 7};
    Value eight{.m_key = 8};
    Slist left{};
    Slist right{};
    left.push_back(five);
    left.push_back(one);
    left.push_back(three);
    right.push_back(six);
    right.push_back(two);
    right.push_back(four);
    const auto compare = [](Value const &left_value, Value const &right_value) noexcept {
        return left_value.m_key < right_value.m_key;
    };
    left.sort(compare);
    right.sort(compare);
    left.merge(right, compare);
    left.reverse();
    const auto removed =
        left.remove_if([](Value const &value) noexcept { return value.m_key % 2 == 0; });
    if (removed != 3U || !right.empty() || two.is_linked() || four.is_linked() || six.is_linked() ||
        left.size() != 3U || left.begin()->m_key != 5) {
        return 5;
    }
    Value duplicate_one{.m_key = 1};
    Value duplicate_two{.m_key = 1};
    Value duplicate_three{.m_key = 2};
    Slist duplicates{};
    duplicates.push_back(duplicate_one);
    duplicates.push_back(duplicate_two);
    duplicates.push_back(duplicate_three);
    if (duplicates.unique([](Value const &left_value, Value const &right_value) noexcept {
            return left_value.m_key == right_value.m_key;
        }) != 1U ||
        duplicate_two.is_linked() || duplicates.size() != 2U) {
        return 6;
    }
    Value clone_one{};
    Value clone_two{};
    Value clone_three{};
    Value failed_clone{};
    Value *const clones[]{&clone_one, &clone_two, &clone_three};
    auto clone_index = 0U;
    auto disposed_clones = 0U;
    Slist copy{};
    const auto copied = copy.clone_from(
        left,
        [&clones, &clone_index](
            Value const &source_value) noexcept -> ustl::Result<Value *, ustl::collections::Error> {
            auto *const clone = clones[clone_index++];
            clone->m_key = source_value.m_key;
            return ustl::ok(clone);
        },
        [&disposed_clones](Value *) noexcept { ++disposed_clones; });
    if (copied.is_err() || copy.size() != 3U || copy.begin()->m_key != 5 || disposed_clones != 0U) {
        return 7;
    }

    Slist failed_copy{};
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
               : 8;
}
