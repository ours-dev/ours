/// Copyright(C) 2026 ours Developers
/// SPDX-License-Identifier: MIT

#ifndef USTL_COLLECTIONS_DETAIL_BTREE_HPP
#define USTL_COLLECTIONS_DETAIL_BTREE_HPP 1

#include <ustl/collections/detail/btree_node_algorithms.hpp>

#include <ours/status.hpp>
#include <ours/types.hpp>

#include <iterator>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace ustl::collections::detail {
    struct BTreeNodeView {
        ours::usize level {};
        ours::usize size {};
        bool leaf {};
    };

    struct NullBTreeObserver {
        template <typename Tree, typename Key>
        constexpr auto on_search(Tree const &, Key const &) -> void {}

        template <typename Tree, typename Value>
        constexpr auto on_insert(Tree const &, Value const &) -> void {}

        template <typename Tree, typename Key>
        constexpr auto on_erase(Tree const &, Key const &) -> void {}

        template <typename Tree, typename NodeView>
        constexpr auto on_node_split(Tree const &, NodeView const &) -> void {}

        template <typename Tree, typename NodeView>
        constexpr auto on_node_merge(Tree const &, NodeView const &) -> void {}

        template <typename Tree, typename NodeView>
        constexpr auto on_node_rebalance(Tree const &, NodeView const &) -> void {}
    };

    struct BTreeDefaultOptions {
        static constexpr auto kTargetNodeSize = ours::usize(256);
        static constexpr auto kAllowDuplicates = false;
    };

    template <typename T>
    struct IdentityKeyOfValue {
        constexpr auto operator()(T const &value) const -> T const & {
            return value;
        }
    };

    template <typename Pair>
    struct SelectFirstKeyOfValue {
        constexpr auto operator()(Pair const &value) const -> decltype((value.first)) {
            return value.first;
        }
    };

    template <
        typename Key,
        typename Value,
        typename KeyOfValue,
        typename Compare,
        typename Allocator,
        typename Observer = NullBTreeObserver,
        typename Options = BTreeDefaultOptions,
        ours::usize Order = priv::kDefaultBTreeOrder
    >
    class BTree {
        using Self = BTree;
        struct Node;
        static_assert(!Options::kAllowDuplicates, "BTree duplicate-key mode is not implemented yet");

      public:
        using key_type = Key;
        using value_type = Value;
        using key_of_value = KeyOfValue;
        using key_compare = Compare;
        using allocator_type = Allocator;
        using observer_type = Observer;
        using options_type = Options;

        using size_type = ours::usize;
        using difference_type = ours::isize;

        using reference = value_type &;
        using const_reference = value_type const &;
        using pointer = value_type *;
        using const_pointer = value_type const *;

        template <bool Const>
        class Iterator {
            using Self = Iterator;
            using TreePtr = std::conditional_t<Const, BTree const *, BTree *>;
            using NodePtr = std::conditional_t<Const, Node const *, Node *>;

          public:
            using iterator_category = std::bidirectional_iterator_tag;
            using value_type = typename BTree::value_type;
            using difference_type = typename BTree::difference_type;
            using pointer = std::conditional_t<Const, typename BTree::const_pointer, typename BTree::pointer>;
            using reference = std::conditional_t<Const, typename BTree::const_reference, typename BTree::reference>;

            constexpr Iterator() = default;

            template <bool OtherConst>
            constexpr Iterator(Iterator<OtherConst> iter) requires(Const && !OtherConst)
                : m_tree(iter.m_tree),
                  m_node(iter.m_node),
                  m_slot(iter.m_slot) {}

            constexpr auto operator*() const -> reference;
            constexpr auto operator->() const -> pointer;

            constexpr auto operator++() -> Self &;
            constexpr auto operator++(int) -> Self;
            constexpr auto operator--() -> Self &;
            constexpr auto operator--(int) -> Self;

            friend constexpr auto operator==(Self const &, Self const &) -> bool = default;

          private:
            friend class BTree;
            template <bool>
            friend class Iterator;

            constexpr Iterator(TreePtr tree, NodePtr node, size_type slot)
                : m_tree(tree),
                  m_node(node),
                  m_slot(slot) {}

            TreePtr m_tree {};
            NodePtr m_node {};
            size_type m_slot {};
        };

        using iterator = Iterator<false>;
        using const_iterator = Iterator<true>;
        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        struct InsertResult {
            iterator iter {};
            bool inserted {};
            ours::Status status {ours::Status::Ok};
        };

        BTree() = default;

        explicit BTree(key_compare const &compare)
            : m_impl(compare) {}

        explicit BTree(allocator_type const &allocator)
            : m_impl(allocator) {}

        BTree(key_compare const &compare, allocator_type const &allocator)
            : m_impl(compare, allocator) {}

        BTree(Self &&other) noexcept
            : m_impl(std::move(other.m_impl)) {
            other.m_impl.root = nullptr;
            other.m_impl.size = 0;
        }

        auto operator=(Self &&other) noexcept -> Self & {
            if (this != &other) {
                clear();
                m_impl = std::move(other.m_impl);
                other.m_impl.root = nullptr;
                other.m_impl.size = 0;
            }
            return *this;
        }

        BTree(Self const &) = delete;
        auto operator=(Self const &) -> Self & = delete;

        ~BTree() {
            clear();
        }

        auto get_allocator() const -> allocator_type {
            return m_impl.allocator;
        }

        auto key_comp() const -> key_compare {
            return m_impl.compare;
        }

        auto observer() -> observer_type & {
            return m_impl.observer;
        }

        auto observer() const -> observer_type const & {
            return m_impl.observer;
        }

        auto begin() -> iterator;
        auto begin() const -> const_iterator;
        auto cbegin() const -> const_iterator;

        auto end() -> iterator;
        auto end() const -> const_iterator;
        auto cend() const -> const_iterator;

        auto rbegin() -> reverse_iterator;
        auto rbegin() const -> const_reverse_iterator;
        auto crbegin() const -> const_reverse_iterator;

        auto rend() -> reverse_iterator;
        auto rend() const -> const_reverse_iterator;
        auto crend() const -> const_reverse_iterator;

        constexpr auto empty() const -> bool {
            return m_impl.size == 0;
        }

        constexpr auto size() const -> size_type {
            return m_impl.size;
        }

        auto max_size() const -> size_type {
            return std::allocator_traits<allocator_type>::max_size(m_impl.allocator);
        }

        auto clear() -> void;

        auto find(key_type const &key) -> iterator;
        auto find(key_type const &key) const -> const_iterator;

        auto lower_bound(key_type const &key) -> iterator;
        auto lower_bound(key_type const &key) const -> const_iterator;

        auto upper_bound(key_type const &key) -> iterator;
        auto upper_bound(key_type const &key) const -> const_iterator;

        auto equal_range(key_type const &key) -> std::pair<iterator, iterator>;
        auto equal_range(key_type const &key) const -> std::pair<const_iterator, const_iterator>;

        auto contains(key_type const &key) const -> bool;
        auto count(key_type const &key) const -> size_type;

        auto insert(value_type const &value) -> InsertResult;
        auto insert(value_type &&value) -> InsertResult;

        template <typename... Args>
        auto emplace(Args &&...args) -> InsertResult;

        auto erase(iterator pos) -> iterator;
        auto erase(iterator first, iterator last) -> iterator;
        auto erase(key_type const &key) -> size_type;

        auto swap(Self &other) noexcept -> void;

      private:
        using NodeAllocator = typename std::allocator_traits<allocator_type>::template rebind_alloc<Node>;
        using NodeAllocatorTraits = std::allocator_traits<NodeAllocator>;

        static constexpr auto kMinDegree = Order;
        static constexpr auto kMaxValues = (Order * 2) - 1;
        static constexpr auto kMaxChildren = Order * 2;

        struct Node {
            explicit Node(bool leaf, Node *parent = nullptr)
                : parent(parent),
                  leaf(leaf) {}

            Node *parent {};
            bool leaf {};
            size_type count {};
            std::optional<value_type> values[kMaxValues] {};
            Node *children[kMaxChildren] {};
        };

        struct NodeTraits {
            using Node = BTree::Node;
            using NodePtr = Node const *;
            using NodePtrMut = Node *;

            static constexpr auto get_parent(NodePtrMut node) -> NodePtrMut {
                return node->parent;
            }

            static constexpr auto get_parent(NodePtr node) -> NodePtr {
                return node->parent;
            }

            static constexpr auto set_parent(NodePtrMut node, NodePtrMut parent) -> void {
                node->parent = parent;
            }

            static constexpr auto get_count(NodePtr node) -> size_type {
                return node->count;
            }

            static constexpr auto is_leaf(NodePtr node) -> bool {
                return node->leaf;
            }

            static constexpr auto get_child(NodePtrMut node, size_type slot) -> NodePtrMut {
                return node->children[slot];
            }

            static constexpr auto get_child(NodePtr node, size_type slot) -> NodePtr {
                return node->children[slot];
            }

            static constexpr auto set_child(NodePtrMut node, size_type slot, NodePtrMut child) -> void {
                node->children[slot] = child;
            }
        };

        using NodeAlgorithms = priv::BTreeNodeAlgorithms<NodeTraits, Order>;

        struct TreeImpl {
            TreeImpl() = default;

            explicit TreeImpl(key_compare const &compare)
                : compare(compare) {}

            explicit TreeImpl(allocator_type const &allocator)
                : allocator(allocator) {}

            TreeImpl(key_compare const &compare, allocator_type const &allocator)
                : compare(compare),
                  allocator(allocator) {}

            [[no_unique_address]] key_compare compare {};
            [[no_unique_address]] allocator_type allocator {};
            [[no_unique_address]] NodeAllocator node_allocator {allocator};
            [[no_unique_address]] key_of_value key_of {};
            [[no_unique_address]] mutable observer_type observer {};
            Node *root {};
            size_type size {};
        };

        static constexpr auto node_view(Node const *node) -> BTreeNodeView {
            return BTreeNodeView {
                .level = 0,
                .size = node == nullptr ? 0 : node->count,
                .leaf = node == nullptr ? true : node->leaf,
            };
        }

        static auto leftmost(BTree *tree, Node *node) -> iterator;
        static auto leftmost(BTree const *tree, Node const *node) -> const_iterator;
        static auto rightmost(BTree *tree, Node *node) -> iterator;
        static auto rightmost(BTree const *tree, Node const *node) -> const_iterator;
        static auto mutable_iterator(BTree *tree, const_iterator iter) -> iterator;

        auto create_node(bool leaf, Node *parent = nullptr) -> Node *;
        auto destroy_node(Node *node) -> void;
        auto destroy_subtree(Node *node) -> void;

        auto key_of(value_type const &value) const -> decltype(auto);
        auto less(key_type const &lhs, key_type const &rhs) const -> bool;
        auto equal(key_type const &lhs, key_type const &rhs) const -> bool;

        auto find_slot(Node const *node, key_type const &key) const -> size_type;
        auto find_node(key_type const &key) -> iterator;
        auto find_node(key_type const &key) const -> const_iterator;

        static auto move_value(std::optional<value_type> &dst, std::optional<value_type> &src) -> void;

        template <typename U>
        auto insert_value(U &&value, bool notify) -> InsertResult;

        template <typename U>
        auto insert_nonfull(Node *root, U &&value) -> iterator;

        auto split_child(Node *parent, size_type slot) -> void;

        auto erase_from_node(Node *node, key_type const &key) -> bool;
        auto erase_from_leaf(Node *node, size_type slot) -> void;
        auto fill_child(Node *node, size_type slot) -> void;
        auto borrow_from_prev(Node *node, size_type slot) -> void;
        auto borrow_from_next(Node *node, size_type slot) -> void;
        auto merge_children(Node *node, size_type slot) -> void;

        TreeImpl m_impl {};
    };

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    template <bool Const>
    constexpr auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::Iterator<Const>::operator*() const -> reference {
        return *m_node->values[m_slot];
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    template <bool Const>
    constexpr auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::Iterator<Const>::operator->() const -> pointer {
        return std::to_address(&*m_node->values[m_slot]);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    template <bool Const>
    constexpr auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::Iterator<Const>::operator++() -> Self & {
        auto const pos = NodeAlgorithms::next_position(m_node, m_slot);
        m_node = pos.node;
        m_slot = pos.slot;
        return *this;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    template <bool Const>
    constexpr auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::Iterator<Const>::operator++(int) -> Self {
        auto old = *this;
        ++*this;
        return old;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    template <bool Const>
    constexpr auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::Iterator<Const>::operator--() -> Self & {
        auto const pos = NodeAlgorithms::prev_position(m_tree->m_impl.root, m_node, m_slot);
        m_node = pos.node;
        m_slot = pos.slot;
        return *this;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    template <bool Const>
    constexpr auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::Iterator<Const>::operator--(int) -> Self {
        auto old = *this;
        --*this;
        return old;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::leftmost(BTree *tree, Node *node) -> iterator {
        return mutable_iterator(
            tree,
            leftmost(static_cast<BTree const *>(tree), static_cast<Node const *>(node))
        );
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::leftmost(BTree const *tree, Node const *node) -> const_iterator {
        if (node == nullptr) {
            return tree->end();
        }
        node = NodeAlgorithms::leftmost(node);
        return const_iterator(tree, node, 0);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::rightmost(BTree *tree, Node *node) -> iterator {
        return mutable_iterator(
            tree,
            rightmost(static_cast<BTree const *>(tree), static_cast<Node const *>(node))
        );
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::rightmost(BTree const *tree, Node const *node) -> const_iterator {
        if (node == nullptr) {
            return tree->end();
        }
        node = NodeAlgorithms::rightmost(node);
        return const_iterator(tree, node, NodeAlgorithms::count(node) - 1);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::mutable_iterator(BTree *tree, const_iterator iter) -> iterator {
        return iterator(tree, const_cast<Node *>(iter.m_node), iter.m_slot);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::create_node(bool leaf, Node *parent) -> Node * {
        auto *node = NodeAllocatorTraits::allocate(m_impl.node_allocator, 1);
        NodeAllocatorTraits::construct(m_impl.node_allocator, node, leaf, parent);
        return node;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::destroy_node(Node *node) -> void {
        NodeAllocatorTraits::destroy(m_impl.node_allocator, node);
        NodeAllocatorTraits::deallocate(m_impl.node_allocator, node, 1);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::destroy_subtree(Node *node) -> void {
        auto *current = node;
        while (current != nullptr) {
            if (auto *child = NodeAlgorithms::first_child(current); child != nullptr) {
                current = child;
                continue;
            }

            auto *parent = NodeAlgorithms::detach_from_parent(current);
            destroy_node(current);
            current = parent;
        }
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::key_of(value_type const &value) const -> decltype(auto) {
        return m_impl.key_of(value);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::less(key_type const &lhs, key_type const &rhs) const -> bool {
        return m_impl.compare(lhs, rhs);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::equal(key_type const &lhs, key_type const &rhs) const -> bool {
        return !less(lhs, rhs) && !less(rhs, lhs);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::find_slot(Node const *node, key_type const &key) const -> size_type {
        size_type slot = 0;
        while (slot < node->count && less(key_of(*node->values[slot]), key)) {
            ++slot;
        }
        return slot;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::begin() -> iterator {
        return mutable_iterator(this, static_cast<Self const *>(this)->begin());
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::begin() const -> const_iterator {
        return leftmost(this, m_impl.root);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::cbegin() const -> const_iterator {
        return begin();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::end() -> iterator {
        return mutable_iterator(this, static_cast<Self const *>(this)->end());
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::end() const -> const_iterator {
        return const_iterator(this, nullptr, 0);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::cend() const -> const_iterator {
        return end();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::rbegin() -> reverse_iterator {
        return reverse_iterator(end());
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::rbegin() const -> const_reverse_iterator {
        return const_reverse_iterator(end());
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::crbegin() const -> const_reverse_iterator {
        return rbegin();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::rend() -> reverse_iterator {
        return reverse_iterator(begin());
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::rend() const -> const_reverse_iterator {
        return const_reverse_iterator(begin());
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::crend() const -> const_reverse_iterator {
        return rend();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::clear() -> void {
        destroy_subtree(m_impl.root);
        m_impl.root = nullptr;
        m_impl.size = 0;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::find_node(key_type const &key) -> iterator {
        return mutable_iterator(this, static_cast<Self const *>(this)->find_node(key));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::find_node(key_type const &key) const -> const_iterator {
        auto const *node = m_impl.root;
        while (node != nullptr) {
            auto const slot = find_slot(node, key);
            if (slot < node->count && equal(key, key_of(*node->values[slot]))) {
                return const_iterator(this, node, slot);
            }
            node = NodeAlgorithms::search_child(node, slot);
        }
        return end();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::find(key_type const &key) -> iterator {
        return mutable_iterator(this, static_cast<Self const *>(this)->find(key));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::find(key_type const &key) const -> const_iterator {
        m_impl.observer.on_search(*this, key);
        return find_node(key);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::lower_bound(key_type const &key) -> iterator {
        return mutable_iterator(this, static_cast<Self const *>(this)->lower_bound(key));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::lower_bound(key_type const &key) const -> const_iterator {
        auto const *node = m_impl.root;
        auto result = end();
        while (node != nullptr) {
            auto const slot = find_slot(node, key);
            if (slot < node->count) {
                result = const_iterator(this, node, slot);
            }
            node = NodeAlgorithms::search_child(node, slot);
        }
        return result;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::upper_bound(key_type const &key) -> iterator {
        return mutable_iterator(this, static_cast<Self const *>(this)->upper_bound(key));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::upper_bound(key_type const &key) const -> const_iterator {
        auto iter = lower_bound(key);
        if (iter != end() && equal(key, key_of(*iter))) {
            ++iter;
        }
        return iter;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::equal_range(key_type const &key) -> std::pair<iterator, iterator> {
        auto const range = static_cast<Self const *>(this)->equal_range(key);
        return {
            mutable_iterator(this, range.first),
            mutable_iterator(this, range.second),
        };
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::equal_range(key_type const &key) const -> std::pair<const_iterator, const_iterator> {
        auto first = lower_bound(key);
        auto last = first;
        if (last != end() && equal(key, key_of(*last))) {
            ++last;
        }
        return {first, last};
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::contains(key_type const &key) const -> bool {
        return find_node(key) != end();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::count(key_type const &key) const -> size_type {
        return contains(key) ? 1 : 0;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::move_value(
        std::optional<value_type> &dst,
        std::optional<value_type> &src
    ) -> void {
        dst.reset();
        dst.emplace(std::move(*src));
        src.reset();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::split_child(Node *parent, size_type slot) -> void {
        auto *full = NodeAlgorithms::child(parent, slot);
        auto *right = create_node(NodeAlgorithms::is_leaf(full), parent);
        right->count = kMinDegree - 1;

        for (size_type i = 0; i < kMinDegree - 1; ++i) {
            move_value(right->values[i], full->values[i + kMinDegree]);
        }

        NodeAlgorithms::move_upper_children_after_split(full, right);

        full->count = kMinDegree - 1;

        NodeAlgorithms::insert_child_after_split(parent, slot, right);

        for (size_type i = parent->count; i > slot; --i) {
            move_value(parent->values[i], parent->values[i - 1]);
        }
        move_value(parent->values[slot], full->values[kMinDegree - 1]);
        ++parent->count;

        m_impl.observer.on_node_split(*this, node_view(full));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    template <typename U>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::insert_nonfull(Node *root, U &&value) -> iterator {
        auto const &key = key_of(value);
        auto *node = root;
        while (true) {
            auto slot = find_slot(node, key);

            if (NodeAlgorithms::is_leaf(node)) {
                for (size_type i = node->count; i > slot; --i) {
                    move_value(node->values[i], node->values[i - 1]);
                }
                node->values[slot].emplace(std::forward<U>(value));
                ++node->count;
                ++m_impl.size;
                return iterator(this, node, slot);
            }

            if (NodeAlgorithms::is_full(NodeAlgorithms::child(node, slot))) {
                split_child(node, slot);
                if (less(key_of(*node->values[slot]), key)) {
                    ++slot;
                }
            }
            node = NodeAlgorithms::child(node, slot);
        }
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    template <typename U>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::insert_value(U &&value, bool notify) -> InsertResult {
        auto const &key = key_of(value);
        if constexpr (!Options::kAllowDuplicates) {
            if (auto iter = find_node(key); iter != end()) {
                return InsertResult {
                    .iter = iter,
                    .inserted = false,
                    .status = ours::Status::AlreadyExists,
                };
            }
        }

        if (m_impl.root == nullptr) {
            m_impl.root = create_node(true);
            m_impl.root->values[0].emplace(std::forward<U>(value));
            m_impl.root->count = 1;
            m_impl.size = 1;
            if (notify) {
                m_impl.observer.on_insert(*this, *m_impl.root->values[0]);
            }
            return InsertResult {
                .iter = iterator(this, m_impl.root, 0),
                .inserted = true,
                .status = ours::Status::Ok,
            };
        }

        if (m_impl.root->count == kMaxValues) {
            auto *root = create_node(false);
            NodeAlgorithms::set_root_child(root, m_impl.root);
            m_impl.root = root;
            split_child(root, 0);
        }

        auto iter = insert_nonfull(m_impl.root, std::forward<U>(value));
        if (notify) {
            m_impl.observer.on_insert(*this, *iter);
        }
        return InsertResult {
            .iter = iter,
            .inserted = true,
            .status = ours::Status::Ok,
        };
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::insert(value_type const &value) -> InsertResult {
        return insert_value(value, true);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::insert(value_type &&value) -> InsertResult {
        return insert_value(std::move(value), true);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    template <typename... Args>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::emplace(Args &&...args) -> InsertResult {
        value_type value(std::forward<Args>(args)...);
        return insert_value(std::move(value), true);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::erase_from_leaf(Node *node, size_type slot) -> void {
        node->values[slot].reset();
        for (size_type i = slot; i + 1 < node->count; ++i) {
            move_value(node->values[i], node->values[i + 1]);
        }
        --node->count;
        --m_impl.size;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::merge_children(Node *node, size_type slot) -> void {
        auto *left = NodeAlgorithms::child(node, slot);
        auto *right = NodeAlgorithms::child(node, slot + 1);

        move_value(left->values[kMinDegree - 1], node->values[slot]);
        for (size_type i = 0; i < right->count; ++i) {
            move_value(left->values[i + kMinDegree], right->values[i]);
        }

        NodeAlgorithms::move_children_for_merge(left, right);

        left->count += right->count + 1;

        for (size_type i = slot; i + 1 < node->count; ++i) {
            move_value(node->values[i], node->values[i + 1]);
        }
        NodeAlgorithms::erase_child_after_merge(node, slot);
        --node->count;

        destroy_node(right);
        m_impl.observer.on_node_merge(*this, node_view(left));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::borrow_from_prev(Node *node, size_type slot) -> void {
        auto *child = NodeAlgorithms::child(node, slot);
        auto *sibling = NodeAlgorithms::child(node, slot - 1);

        for (size_type i = child->count; i > 0; --i) {
            move_value(child->values[i], child->values[i - 1]);
        }
        NodeAlgorithms::move_last_child_to_front(child, sibling);

        move_value(child->values[0], node->values[slot - 1]);
        move_value(node->values[slot - 1], sibling->values[sibling->count - 1]);
        ++child->count;
        --sibling->count;
        m_impl.observer.on_node_rebalance(*this, node_view(child));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::borrow_from_next(Node *node, size_type slot) -> void {
        auto *child = NodeAlgorithms::child(node, slot);
        auto *sibling = NodeAlgorithms::child(node, slot + 1);

        move_value(child->values[child->count], node->values[slot]);
        NodeAlgorithms::move_first_child_to_back(child, sibling);

        move_value(node->values[slot], sibling->values[0]);
        for (size_type i = 0; i + 1 < sibling->count; ++i) {
            move_value(sibling->values[i], sibling->values[i + 1]);
        }

        ++child->count;
        --sibling->count;
        m_impl.observer.on_node_rebalance(*this, node_view(child));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::fill_child(Node *node, size_type slot) -> void {
        switch (NodeAlgorithms::select_rebalance_plan(node, slot)) {
        case NodeAlgorithms::RebalancePlan::BorrowFromPrev:
            borrow_from_prev(node, slot);
            break;
        case NodeAlgorithms::RebalancePlan::BorrowFromNext:
            borrow_from_next(node, slot);
            break;
        case NodeAlgorithms::RebalancePlan::MergeWithNext:
            merge_children(node, slot);
            break;
        case NodeAlgorithms::RebalancePlan::MergeWithPrev:
            merge_children(node, slot - 1);
            break;
        }
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::erase_from_node(Node *node, key_type const &key) -> bool {
        auto current_key = key;
        auto *current = node;

        while (current != nullptr) {
            auto slot = find_slot(current, current_key);
            if (slot < current->count && equal(current_key, key_of(*current->values[slot]))) {
                if (NodeAlgorithms::is_leaf(current)) {
                    erase_from_leaf(current, slot);
                    return true;
                }

                auto *left = NodeAlgorithms::child(current, slot);
                auto *right = NodeAlgorithms::child(current, slot + 1);

                if (NodeAlgorithms::has_min_degree(left)) {
                    auto pred = rightmost(this, left);
                    current_key = key_of(*pred);
                    move_value(current->values[slot], pred.m_node->values[pred.m_slot]);
                    current = left;
                    continue;
                }

                if (NodeAlgorithms::has_min_degree(right)) {
                    auto succ = leftmost(this, right);
                    current_key = key_of(*succ);
                    move_value(current->values[slot], succ.m_node->values[succ.m_slot]);
                    current = right;
                    continue;
                }

                current_key = key_of(*current->values[slot]);
                merge_children(current, slot);
                current = left;
                continue;
            }

            if (NodeAlgorithms::is_leaf(current)) {
                return false;
            }

            auto const was_last = slot == current->count;
            if (NodeAlgorithms::needs_fill(NodeAlgorithms::child(current, slot))) {
                fill_child(current, slot);
                if (was_last && slot > current->count) {
                    --slot;
                }
            }
            current = NodeAlgorithms::child(current, slot);
        }

        return false;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::erase(key_type const &key) -> size_type {
        if (m_impl.root == nullptr) {
            return 0;
        }

        m_impl.observer.on_erase(*this, key);
        auto const erased = erase_from_node(m_impl.root, key);
        if (!erased) {
            return 0;
        }

        if (m_impl.root->count == 0) {
            auto *old_root = m_impl.root;
            m_impl.root = NodeAlgorithms::take_first_child(old_root);
            destroy_node(old_root);
        }
        return 1;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::erase(iterator pos) -> iterator {
        if (pos == end()) {
            return end();
        }
        auto next = pos;
        ++next;
        std::optional<key_type> next_key;
        if (next != end()) {
            next_key.emplace(key_of(*next));
        }
        key_type const key = key_of(*pos);
        erase(key);
        return next_key.has_value() ? lower_bound(*next_key) : end();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::erase(iterator first, iterator last) -> iterator {
        if (last == end()) {
            while (first != end()) {
                first = erase(first);
            }
            return end();
        }

        key_type const last_key = key_of(*last);
        while (first != end() && !equal(key_of(*first), last_key)) {
            first = erase(first);
        }
        return lower_bound(last_key);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, ours::usize Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::swap(Self &other) noexcept -> void {
        using std::swap;
        swap(m_impl, other.m_impl);
    }
} // namespace ustl::collections::detail

#endif // #ifndef USTL_COLLECTIONS_DETAIL_BTREE_HPP
