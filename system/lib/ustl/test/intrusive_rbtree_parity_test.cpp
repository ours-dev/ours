/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <concepts>

#include <cstddef>

#include <ustl/collections/intrusive/rbtree.hpp>

namespace {
struct Value final: ustl::collections::intrusive::SetBaseHook<> {
    int m_key{};
};

struct Compare final {
    [[nodiscard]]
    constexpr auto operator()(Value const &left, Value const &right) const noexcept -> bool {
        return left.m_key < right.m_key;
    }
};

using Tree =
    ustl::collections::intrusive::RBTree<Value, ustl::collections::intrusive::Compare<Compare>>;

template <typename T>
concept HasLegacyCollectionAliases = requires {
    typename T::ValueType;
    typename T::SizeType;
    typename T::Iterator;
    typename T::ConstIterator;
};

template <typename T>
concept HasLegacyIteratorAliases = requires {
    typename T::ValueType;
    typename T::Pointer;
    typename T::Reference;
    typename T::value_type;
    typename T::pointer;
    typename T::reference;
};

static_assert(std::same_as<Tree::Element, Value>);
static_assert(std::same_as<Tree::Size, std::size_t>);
static_assert(std::same_as<Tree::IterMut::Element, Value>);
static_assert(std::same_as<Tree::IterMut::RefMut, Value &>);
static_assert(std::same_as<Tree::IterMut::PtrMut, Value *>);
static_assert(std::same_as<Tree::Iter::Ref, Value const &>);
static_assert(std::same_as<Tree::Iter::Ptr, Value const *>);
static_assert(!HasLegacyCollectionAliases<Tree>);
static_assert(!HasLegacyIteratorAliases<Tree::IterMut>);
} // namespace

auto main() -> int {
    Value one{.m_key = 1};
    Value two{.m_key = 2};
    Value three{.m_key = 3};
    Value replacement{.m_key = 2};
    Tree tree{};
    static_cast<void>(tree.insert_equal(one));
    static_cast<void>(tree.insert_equal(two));
    static_cast<void>(tree.insert_before(tree.end(), three));

    Tree const &const_tree = tree;
    if (const_tree.cbegin()->m_key != 1 || const_tree.crbegin()->m_key != 3 ||
        !const_tree.upper_bound(two)) {
        return 1;
    }

    tree.replace_node(tree.iterator_to(two), replacement);
    if (two.is_linked() || !replacement.is_linked() || tree.iterator_to(replacement)->m_key != 2) {
        return 2;
    }

    const auto successor = tree.erase(tree.iterator_to(one));
    if (successor->m_key != 2 || one.is_linked() || tree.size() != 2U) {
        return 3;
    }

    auto disposed = 0U;
    tree.clear_and_dispose([&disposed](Value *const value) noexcept {
        disposed += static_cast<unsigned>(value->m_key >= 2);
    });
    return tree.empty() && !replacement.is_linked() && !three.is_linked() && disposed == 2U ? 0 : 4;
}
