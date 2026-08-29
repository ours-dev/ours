/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/detail/tree_iterator.hpp>
#include <ustl/collections/intrusive/detail/tree_node.hpp>

namespace {
struct Item final: ustl::collections::intrusive::TreeNode<void *> {
    int m_value{};
};

struct StaticValueTraits final {
    using NodeTraits = ustl::collections::intrusive::TreeNodeTraits<void *>;
    using NodePtr = typename NodeTraits::NodePtr;
    using Element = Item;
    using PtrMut = Item *;
    using Ptr = Item const *;

    static constexpr bool IS_STATEFUL_VALUE_TRAITS = false;

    [[nodiscard]]
    static constexpr auto to_value_ptr(NodePtr const node) noexcept -> PtrMut {
        return static_cast<PtrMut>(node);
    }
};

using IterMut = ustl::collections::intrusive::detail::TreeIter<StaticValueTraits, false>;
using Iter = ustl::collections::intrusive::detail::TreeIter<StaticValueTraits, true>;
} // namespace

auto main() -> int {
    Item header{};
    Item root{};
    Item left{};
    Item right{};
    left.m_value = 1;
    root.m_value = 2;
    right.m_value = 3;

    StaticValueTraits::NodeTraits::set_parent(&header, &root);
    StaticValueTraits::NodeTraits::set_left(&header, &left);
    StaticValueTraits::NodeTraits::set_right(&header, &right);
    StaticValueTraits::NodeTraits::set_parent(&root, &header);
    StaticValueTraits::NodeTraits::set_left(&root, &left);
    StaticValueTraits::NodeTraits::set_right(&root, &right);
    StaticValueTraits::NodeTraits::set_parent(&left, &root);
    StaticValueTraits::NodeTraits::set_left(&left, nullptr);
    StaticValueTraits::NodeTraits::set_right(&left, nullptr);
    StaticValueTraits::NodeTraits::set_parent(&right, &root);
    StaticValueTraits::NodeTraits::set_left(&right, nullptr);
    StaticValueTraits::NodeTraits::set_right(&right, nullptr);

    IterMut iterator(&left);
    if (iterator->m_value != 1 || (++iterator)->m_value != 2 || (++iterator)->m_value != 3 ||
        (++iterator).pointed_node() != &header || (--iterator).pointed_node() != &right) {
        return 1;
    }

    IterMut root_iterator(&root);
    if (root_iterator.go_left().pointed_node() != &left ||
        root_iterator.go_parent().pointed_node() != &root ||
        root_iterator.go_right().pointed_node() != &right ||
        root_iterator.end_iterator_from_it().pointed_node() != &header) {
        return 2;
    }

    Iter const_iterator(root_iterator);
    return const_iterator->m_value == 3 && const_iterator.unconst() == root_iterator ? 0 : 3;
}
