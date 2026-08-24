/// Copyright(C) 2026 ours Developers
/// SPDX-License-Identifier: MIT

#ifndef USTL_COLLECTIONS_DETAIL_BTREE_HPP
#define USTL_COLLECTIONS_DETAIL_BTREE_HPP 1

#include <ustl/collections/allocator.hpp>
#include <ustl/collections/detail/btree_node_algorithms.hpp>
#include <ustl/collections/error.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

#include <cstddef>
#include <concepts>
#include <iterator>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ustl::collections::detail {
    struct BTreeNodeView {
        std::size_t m_level {};
        std::size_t m_size {};
        bool m_leaf {};
    };

    struct NullBTreeObserver {
        template <typename Tree, typename Key>
        constexpr auto on_search(Tree const &, Key const &) noexcept -> void {}

        template <typename Tree, typename Value>
        constexpr auto on_insert(Tree const &, Value const &) noexcept -> void {}

        template <typename Tree, typename Key>
        constexpr auto on_erase(Tree const &, Key const &) noexcept -> void {}

        template <typename Tree, typename NodeView>
        constexpr auto on_node_split(Tree const &, NodeView const &) noexcept -> void {}

        template <typename Tree, typename NodeView>
        constexpr auto on_node_merge(Tree const &, NodeView const &) noexcept -> void {}

        template <typename Tree, typename NodeView>
        constexpr auto on_node_rebalance(Tree const &, NodeView const &) noexcept -> void {}
    };

    struct BTreeDefaultOptions {
        static constexpr auto TARGET_NODE_SIZE = std::size_t(256);
        static constexpr auto ALLOW_DUPLICATES = false;
    };

    template <typename T>
    struct IdentityKeyOfValue {
        constexpr auto operator()(T const &value) const noexcept -> T const & {
            return value;
        }
    };

    template <typename Pair>
    struct SelectFirstKeyOfValue {
        constexpr auto operator()(Pair const &value) const noexcept -> decltype((value.first)) {
            return value.first;
        }
    };

    template <typename T>
    struct DefaultCompare final {
        [[nodiscard]] constexpr auto operator()(T const &left, T const &right) const noexcept(noexcept(left < right))
            -> bool {
            return left < right;
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
        std::size_t Order = 8
    >
    class BTree {
        using Self = BTree;
        struct Node;
        static_assert(!Options::ALLOW_DUPLICATES, "BTree duplicate-key mode is not implemented yet");
        static_assert(std::is_nothrow_destructible_v<Value>, "BTree value types must be nothrow destructible.");
        static_assert(std::is_nothrow_move_constructible_v<Value>, "BTree value types must be nothrow move constructible.");
        static_assert(std::is_nothrow_invocable_r_v<bool, Compare const &, Key const &, Key const &>,
                      "BTree comparators must be noexcept.");
        static_assert(std::is_nothrow_invocable_v<KeyOfValue const &, Value const &>,
                      "BTree key extractors must be noexcept.");
        static_assert(requires(BTree const &tree, Observer &observer, Key const &key, Value const &value,
                               BTreeNodeView const &view) {
            { observer.on_search(tree, key) } noexcept;
            { observer.on_insert(tree, value) } noexcept;
            { observer.on_erase(tree, key) } noexcept;
            { observer.on_node_split(tree, view) } noexcept;
            { observer.on_node_merge(tree, view) } noexcept;
            { observer.on_node_rebalance(tree, view) } noexcept;
        }, "BTree observers must be noexcept.");

      public:
        using key_type = Key;
        using value_type = Value;
        using key_of_value = KeyOfValue;
        using key_compare = Compare;
        using allocator_type = Allocator;
        using observer_type = Observer;
        using options_type = Options;

        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;

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

        using InsertResult = std::pair<iterator, bool>;

        BTree() = default;

        explicit BTree(key_compare const &compare)
            : m_impl(compare) {}

        explicit BTree(allocator_type const &allocator)
            : m_impl(allocator) {}

        BTree(key_compare const &compare, allocator_type const &allocator)
            : m_impl(compare, allocator) {}

        BTree(Self &&other) noexcept
            : m_impl(std::move(other.m_impl)) {
            other.m_impl.m_root = nullptr;
            other.m_impl.m_size = 0;
        }

        auto operator=(Self &&other) noexcept -> Self & {
            if (this != &other) {
                clear();
                m_impl = std::move(other.m_impl);
                other.m_impl.m_root = nullptr;
                other.m_impl.m_size = 0;
            }
            return *this;
        }

        BTree(Self const &) = delete;
        auto operator=(Self const &) -> Self & = delete;

        ~BTree() {
            clear();
        }

        auto get_allocator() const -> allocator_type {
            return m_impl.m_allocator;
        }

        auto key_comp() const -> key_compare {
            return m_impl.m_compare;
        }

        auto observer() -> observer_type & {
            return m_impl.m_observer;
        }

        auto observer() const -> observer_type const & {
            return m_impl.m_observer;
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
            return m_impl.m_size == 0;
        }

        constexpr auto size() const -> size_type {
            return m_impl.m_size;
        }

        auto max_size() const noexcept -> size_type {
            return NodeAllocator::max_size();
        }

        auto clear() -> void;

        auto find(key_type const &key) noexcept -> Option<iterator>;
        auto find(key_type const &key) const noexcept -> Option<const_iterator>;

        auto lower_bound(key_type const &key) noexcept -> Option<iterator>;
        auto lower_bound(key_type const &key) const noexcept -> Option<const_iterator>;

        auto upper_bound(key_type const &key) noexcept -> Option<iterator>;
        auto upper_bound(key_type const &key) const noexcept -> Option<const_iterator>;

        auto equal_range(key_type const &key) -> std::pair<iterator, iterator>;
        auto equal_range(key_type const &key) const -> std::pair<const_iterator, const_iterator>;

        auto contains(key_type const &key) const -> bool;
        auto count(key_type const &key) const -> size_type;

        auto insert(value_type const &value) noexcept -> Result<InsertResult, Error>;
        auto insert(value_type &&value) noexcept -> Result<InsertResult, Error>;

        template <typename... Args>
        auto emplace(Args &&...args) noexcept -> Result<InsertResult, Error>;

        template <typename... Args>
            requires requires { typename value_type::second_type; }
                     && std::is_nothrow_constructible_v<value_type, std::piecewise_construct_t,
                                                        std::tuple<key_type const &>, std::tuple<Args &&...>>
        auto try_emplace(key_type const &key, Args &&...args) noexcept -> Result<InsertResult, Error>;

        template <typename Mapped>
            requires requires(value_type &stored, Mapped &&mapped) {
                typename value_type::second_type;
                { stored.second = std::forward<Mapped>(mapped) } noexcept;
            }
        auto insert_or_assign(key_type const &key, Mapped &&mapped) noexcept -> Result<InsertResult, Error>;

        auto erase(iterator pos) -> iterator;
        auto erase(iterator first, iterator last) -> iterator;
        auto erase(key_type const &key) -> size_type;

        auto swap(Self &other) noexcept -> void;

      private:
        using NodeAllocator = typename allocator_type::template Rebind<Node>;

        static_assert(requires(NodeAllocator &allocator, Node *node) {
            { allocator.try_allocate(1) } noexcept -> std::same_as<Result<Node *, Error>>;
            { allocator.deallocate(node, 1) } noexcept -> std::same_as<void>;
            { NodeAllocator::max_size() } noexcept -> std::same_as<std::size_t>;
        }, "BTree allocators must use the USTL fallible allocation interface.");

        static constexpr auto K_MIN_DEGREE = Order;
        static constexpr auto K_MAX_VALUES = (Order * 2) - 1;
        static constexpr auto K_MAX_CHILDREN = Order * 2;

        struct Node {
            explicit Node(bool leaf, Node *parent = nullptr)
                : m_parent(parent),
                  m_leaf(leaf) {}

            Node *m_parent {};
            bool m_leaf {};
            size_type m_count {};
            std::optional<value_type> m_values[K_MAX_VALUES] {};
            Node *m_children[K_MAX_CHILDREN] {};
        };

        struct NodeTraits {
            using Node = BTree::Node;
            using NodePtr = Node const *;
            using NodePtrMut = Node *;

            static constexpr auto get_parent(NodePtrMut node) -> NodePtrMut {
                return node->m_parent;
            }

            static constexpr auto get_parent(NodePtr node) -> NodePtr {
                return node->m_parent;
            }

            static constexpr auto set_parent(NodePtrMut node, NodePtrMut parent) -> void {
                node->m_parent = parent;
            }

            static constexpr auto get_count(NodePtr node) -> size_type {
                return node->m_count;
            }

            static constexpr auto is_leaf(NodePtr node) -> bool {
                return node->m_leaf;
            }

            static constexpr auto get_child(NodePtrMut node, size_type slot) -> NodePtrMut {
                return node->m_children[slot];
            }

            static constexpr auto get_child(NodePtr node, size_type slot) -> NodePtr {
                return node->m_children[slot];
            }

            static constexpr auto set_child(NodePtrMut node, size_type slot, NodePtrMut child) -> void {
                node->m_children[slot] = child;
            }
        };

        using NodeAlgorithms = priv::BTreeNodeAlgorithms<NodeTraits, Order>;

        struct TreeImpl {
            TreeImpl() = default;

            explicit TreeImpl(key_compare const &compare)
                : m_compare(compare) {}

            explicit TreeImpl(allocator_type const &allocator)
                : m_allocator(allocator) {}

            TreeImpl(key_compare const &compare, allocator_type const &allocator)
                : m_compare(compare),
                  m_allocator(allocator) {}

            [[no_unique_address]] key_compare m_compare {};
            [[no_unique_address]] allocator_type m_allocator {};
            [[no_unique_address]] NodeAllocator m_node_allocator {*m_allocator.resource()};
            [[no_unique_address]] key_of_value m_key_of {};
            [[no_unique_address]] mutable observer_type m_observer {};
            Node *m_root {};
            size_type m_size {};
        };

        static constexpr auto node_view(Node const *node) -> BTreeNodeView {
            return BTreeNodeView {
                .m_level = 0,
                .m_size = node == nullptr ? 0 : node->m_count,
                .m_leaf = node == nullptr ? true : node->m_leaf,
            };
        }

        static auto leftmost(BTree *tree, Node *node) -> iterator;
        static auto leftmost(BTree const *tree, Node const *node) -> const_iterator;
        static auto rightmost(BTree *tree, Node *node) -> iterator;
        static auto rightmost(BTree const *tree, Node const *node) -> const_iterator;
        static auto mutable_iterator(BTree *tree, const_iterator iter) -> iterator;

        auto create_node(bool leaf, Node *parent = nullptr) noexcept -> Result<Node *, Error>;
        auto destroy_node(Node *node) noexcept -> void;
        auto destroy_subtree(Node *node) noexcept -> void;

        auto key_of(value_type const &value) const -> decltype(auto);
        auto less(key_type const &lhs, key_type const &rhs) const -> bool;
        auto equal(key_type const &lhs, key_type const &rhs) const -> bool;

        auto find_slot(Node const *node, key_type const &key) const -> size_type;
        auto find_node(key_type const &key) -> iterator;
        auto find_node(key_type const &key) const -> const_iterator;
        auto lower_bound_node(key_type const &key) -> iterator;
        auto lower_bound_node(key_type const &key) const -> const_iterator;
        auto upper_bound_node(key_type const &key) -> iterator;
        auto upper_bound_node(key_type const &key) const -> const_iterator;

        static auto move_value(std::optional<value_type> &dst, std::optional<value_type> &src) -> void;

        template <typename U>
        auto insert_value(U &&value, bool notify) noexcept -> Result<InsertResult, Error>;

        template <typename U>
        auto insert_nonfull(Node *root, U &&value) noexcept -> Result<iterator, Error>;

        auto split_child(Node *parent, size_type slot, Node *right) noexcept -> void;

        auto erase_from_node(Node *node, key_type const &key) -> bool;
        auto erase_from_leaf(Node *node, size_type slot) -> void;
        auto fill_child(Node *node, size_type slot) -> void;
        auto borrow_from_prev(Node *node, size_type slot) -> void;
        auto borrow_from_next(Node *node, size_type slot) -> void;
        auto merge_children(Node *node, size_type slot) -> void;

        TreeImpl m_impl {};
    };

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    template <bool Const>
    constexpr auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::Iterator<Const>::operator*() const -> reference {
        return *m_node->m_values[m_slot];
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    template <bool Const>
    constexpr auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::Iterator<Const>::operator->() const -> pointer {
        return std::to_address(&*m_node->m_values[m_slot]);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    template <bool Const>
    constexpr auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::Iterator<Const>::operator++() -> Self & {
        auto const pos = NodeAlgorithms::next_position(m_node, m_slot);
        m_node = pos.m_node;
        m_slot = pos.m_slot;
        return *this;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    template <bool Const>
    constexpr auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::Iterator<Const>::operator++(int) -> Self {
        auto old = *this;
        ++*this;
        return old;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    template <bool Const>
    constexpr auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::Iterator<Const>::operator--() -> Self & {
        auto const pos = NodeAlgorithms::prev_position(m_tree->m_impl.m_root, m_node, m_slot);
        m_node = pos.m_node;
        m_slot = pos.m_slot;
        return *this;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    template <bool Const>
    constexpr auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::Iterator<Const>::operator--(int) -> Self {
        auto old = *this;
        --*this;
        return old;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::leftmost(BTree *tree, Node *node) -> iterator {
        return mutable_iterator(
            tree,
            leftmost(static_cast<BTree const *>(tree), static_cast<Node const *>(node))
        );
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::leftmost(BTree const *tree, Node const *node) -> const_iterator {
        if (node == nullptr) {
            return tree->end();
        }
        node = NodeAlgorithms::leftmost(node);
        return const_iterator(tree, node, 0);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::rightmost(BTree *tree, Node *node) -> iterator {
        return mutable_iterator(
            tree,
            rightmost(static_cast<BTree const *>(tree), static_cast<Node const *>(node))
        );
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::rightmost(BTree const *tree, Node const *node) -> const_iterator {
        if (node == nullptr) {
            return tree->end();
        }
        node = NodeAlgorithms::rightmost(node);
        return const_iterator(tree, node, NodeAlgorithms::count(node) - 1);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::mutable_iterator(BTree *tree, const_iterator iter) -> iterator {
        return iterator(tree, const_cast<Node *>(iter.m_node), iter.m_slot);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::create_node(
        bool leaf, Node *parent
    ) noexcept -> Result<Node *, Error> {
        auto allocation = m_impl.m_node_allocator.try_allocate(1);
        if (allocation.is_err()) {
            return err(allocation.error());
        }

        auto *const node = allocation.value();
        std::construct_at(node, leaf, parent);
        return ok(node);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::destroy_node(Node *node) noexcept -> void {
        std::destroy_at(node);
        m_impl.m_node_allocator.deallocate(node, 1);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::destroy_subtree(
        Node *node
    ) noexcept -> void {
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
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::key_of(value_type const &value) const -> decltype(auto) {
        return m_impl.m_key_of(value);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::less(key_type const &lhs, key_type const &rhs) const -> bool {
        return m_impl.m_compare(lhs, rhs);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::equal(key_type const &lhs, key_type const &rhs) const -> bool {
        return !less(lhs, rhs) && !less(rhs, lhs);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::find_slot(Node const *node, key_type const &key) const -> size_type {
        size_type slot = 0;
        while (slot < node->m_count && less(key_of(*node->m_values[slot]), key)) {
            ++slot;
        }
        return slot;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::begin() -> iterator {
        return mutable_iterator(this, static_cast<Self const *>(this)->begin());
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::begin() const -> const_iterator {
        return leftmost(this, m_impl.m_root);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::cbegin() const -> const_iterator {
        return begin();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::end() -> iterator {
        return mutable_iterator(this, static_cast<Self const *>(this)->end());
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::end() const -> const_iterator {
        return const_iterator(this, nullptr, 0);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::cend() const -> const_iterator {
        return end();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::rbegin() -> reverse_iterator {
        return reverse_iterator(end());
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::rbegin() const -> const_reverse_iterator {
        return const_reverse_iterator(end());
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::crbegin() const -> const_reverse_iterator {
        return rbegin();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::rend() -> reverse_iterator {
        return reverse_iterator(begin());
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::rend() const -> const_reverse_iterator {
        return const_reverse_iterator(begin());
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::crend() const -> const_reverse_iterator {
        return rend();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::clear() -> void {
        destroy_subtree(m_impl.m_root);
        m_impl.m_root = nullptr;
        m_impl.m_size = 0;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::find_node(key_type const &key) -> iterator {
        return mutable_iterator(this, static_cast<Self const *>(this)->find_node(key));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::find_node(key_type const &key) const -> const_iterator {
        auto const *node = m_impl.m_root;
        while (node != nullptr) {
            auto const slot = find_slot(node, key);
            if (slot < node->m_count && equal(key, key_of(*node->m_values[slot]))) {
                return const_iterator(this, node, slot);
            }
            node = NodeAlgorithms::search_child(node, slot);
        }
        return end();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::find(
        key_type const &key
    ) noexcept -> Option<iterator> {
        m_impl.m_observer.on_search(*this, key);
        auto const result = find_node(key);
        return result == end() ? NONE : Option<iterator>(result);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::find(
        key_type const &key
    ) const noexcept -> Option<const_iterator> {
        m_impl.m_observer.on_search(*this, key);
        auto const result = find_node(key);
        return result == end() ? NONE : Option<const_iterator>(result);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::lower_bound_node(
        key_type const &key
    ) -> iterator {
        return mutable_iterator(this, static_cast<Self const *>(this)->lower_bound_node(key));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::lower_bound_node(
        key_type const &key
    ) const -> const_iterator {
        auto const *node = m_impl.m_root;
        auto result = end();
        while (node != nullptr) {
            auto const slot = find_slot(node, key);
            if (slot < node->m_count) {
                result = const_iterator(this, node, slot);
            }
            node = NodeAlgorithms::search_child(node, slot);
        }
        return result;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::lower_bound(
        key_type const &key
    ) noexcept -> Option<iterator> {
        auto const result = lower_bound_node(key);
        return result == end() ? NONE : Option<iterator>(result);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::lower_bound(
        key_type const &key
    ) const noexcept -> Option<const_iterator> {
        auto const result = lower_bound_node(key);
        return result == end() ? NONE : Option<const_iterator>(result);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::upper_bound_node(
        key_type const &key
    ) -> iterator {
        return mutable_iterator(this, static_cast<Self const *>(this)->upper_bound_node(key));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::upper_bound_node(
        key_type const &key
    ) const -> const_iterator {
        auto iter = lower_bound_node(key);
        if (iter != end() && equal(key, key_of(*iter))) {
            ++iter;
        }
        return iter;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::upper_bound(
        key_type const &key
    ) noexcept -> Option<iterator> {
        auto const result = upper_bound_node(key);
        return result == end() ? NONE : Option<iterator>(result);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::upper_bound(
        key_type const &key
    ) const noexcept -> Option<const_iterator> {
        auto const result = upper_bound_node(key);
        return result == end() ? NONE : Option<const_iterator>(result);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::equal_range(key_type const &key) -> std::pair<iterator, iterator> {
        auto const range = static_cast<Self const *>(this)->equal_range(key);
        return {
            mutable_iterator(this, range.first),
            mutable_iterator(this, range.second),
        };
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::equal_range(key_type const &key) const -> std::pair<const_iterator, const_iterator> {
        auto first = lower_bound_node(key);
        auto last = first;
        if (last != end() && equal(key, key_of(*last))) {
            ++last;
        }
        return {first, last};
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::contains(key_type const &key) const -> bool {
        return find_node(key) != end();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::count(key_type const &key) const -> size_type {
        return contains(key) ? 1 : 0;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::move_value(
        std::optional<value_type> &dst,
        std::optional<value_type> &src
    ) -> void {
        dst.reset();
        dst.emplace(std::move(*src));
        src.reset();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::split_child(
        Node *parent, size_type slot, Node *right
    ) noexcept -> void {
        auto *full = NodeAlgorithms::child(parent, slot);
        right->m_count = K_MIN_DEGREE - 1;

        for (size_type i = 0; i < K_MIN_DEGREE - 1; ++i) {
            move_value(right->m_values[i], full->m_values[i + K_MIN_DEGREE]);
        }

        NodeAlgorithms::move_upper_children_after_split(full, right);

        full->m_count = K_MIN_DEGREE - 1;

        NodeAlgorithms::insert_child_after_split(parent, slot, right);

        for (size_type i = parent->m_count; i > slot; --i) {
            move_value(parent->m_values[i], parent->m_values[i - 1]);
        }
        move_value(parent->m_values[slot], full->m_values[K_MIN_DEGREE - 1]);
        ++parent->m_count;

        m_impl.m_observer.on_node_split(*this, node_view(full));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    template <typename U>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::insert_nonfull(
        Node *root, U &&value
    ) noexcept -> Result<iterator, Error> {
        auto const &key = key_of(value);
        auto *node = root;
        while (true) {
            auto slot = find_slot(node, key);

            if (NodeAlgorithms::is_leaf(node)) {
                for (size_type i = node->m_count; i > slot; --i) {
                    move_value(node->m_values[i], node->m_values[i - 1]);
                }
                node->m_values[slot].emplace(std::forward<U>(value));
                ++node->m_count;
                ++m_impl.m_size;
                return ok(iterator(this, node, slot));
            }

            if (NodeAlgorithms::is_full(NodeAlgorithms::child(node, slot))) {
                auto allocation = create_node(NodeAlgorithms::is_leaf(NodeAlgorithms::child(node, slot)), node);
                if (allocation.is_err()) {
                    return err(allocation.error());
                }
                split_child(node, slot, allocation.value());
                if (less(key_of(*node->m_values[slot]), key)) {
                    ++slot;
                }
            }
            node = NodeAlgorithms::child(node, slot);
        }
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    template <typename U>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::insert_value(
        U &&value, bool notify
    ) noexcept -> Result<InsertResult, Error> {
        auto const &key = key_of(value);
        if constexpr (!Options::ALLOW_DUPLICATES) {
            if (auto iter = find_node(key); iter != end()) {
                return ok(InsertResult {iter, false});
            }
        }

        if (m_impl.m_root == nullptr) {
            auto allocation = create_node(true);
            if (allocation.is_err()) {
                return err(allocation.error());
            }
            m_impl.m_root = allocation.value();
            m_impl.m_root->m_values[0].emplace(std::forward<U>(value));
            m_impl.m_root->m_count = 1;
            m_impl.m_size = 1;
            if (notify) {
                m_impl.m_observer.on_insert(*this, *m_impl.m_root->m_values[0]);
            }
            return ok(InsertResult {iterator(this, m_impl.m_root, 0), true});
        }

        if (m_impl.m_root->m_count == K_MAX_VALUES) {
            auto root_allocation = create_node(false);
            if (root_allocation.is_err()) {
                return err(root_allocation.error());
            }
            auto *const root = root_allocation.value();
            auto right_allocation = create_node(NodeAlgorithms::is_leaf(m_impl.m_root), root);
            if (right_allocation.is_err()) {
                destroy_node(root);
                return err(right_allocation.error());
            }
            NodeAlgorithms::set_root_child(root, m_impl.m_root);
            m_impl.m_root = root;
            split_child(root, 0, right_allocation.value());
        }

        auto inserted = insert_nonfull(m_impl.m_root, std::forward<U>(value));
        if (inserted.is_err()) {
            return err(inserted.error());
        }
        auto iter = inserted.value();
        if (notify) {
            m_impl.m_observer.on_insert(*this, *iter);
        }
        return ok(InsertResult {iter, true});
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::insert(
        value_type const &value
    ) noexcept -> Result<InsertResult, Error> {
        return insert_value(value, true);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::insert(
        value_type &&value
    ) noexcept -> Result<InsertResult, Error> {
        return insert_value(std::move(value), true);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    template <typename... Args>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::emplace(
        Args &&...args
    ) noexcept -> Result<InsertResult, Error> {
        value_type value(std::forward<Args>(args)...);
        return insert_value(std::move(value), true);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    template <typename... Args>
        requires requires { typename BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::value_type::second_type; }
                 && std::is_nothrow_constructible_v<
                     Value, std::piecewise_construct_t, std::tuple<Key const &>, std::tuple<Args &&...>>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::try_emplace(
        key_type const &key, Args &&...args
    ) noexcept -> Result<InsertResult, Error> {
        auto const existing = find_node(key);
        if (existing != end()) {
            return ok(InsertResult {existing, false});
        }

        value_type value(
            std::piecewise_construct,
            std::forward_as_tuple(key),
            std::forward_as_tuple(std::forward<Args>(args)...)
        );
        return insert_value(std::move(value), true);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    template <typename Mapped>
        requires requires(Value &stored, Mapped &&mapped) {
            typename Value::second_type;
            { stored.second = std::forward<Mapped>(mapped) } noexcept;
        }
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::insert_or_assign(
        key_type const &key, Mapped &&mapped
    ) noexcept -> Result<InsertResult, Error> {
        auto const existing = find_node(key);
        if (existing != end()) {
            existing->second = std::forward<Mapped>(mapped);
            return ok(InsertResult {existing, false});
        }
        return try_emplace(key, std::forward<Mapped>(mapped));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::erase_from_leaf(Node *node, size_type slot) -> void {
        node->m_values[slot].reset();
        for (size_type i = slot; i + 1 < node->m_count; ++i) {
            move_value(node->m_values[i], node->m_values[i + 1]);
        }
        --node->m_count;
        --m_impl.m_size;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::merge_children(Node *node, size_type slot) -> void {
        auto *left = NodeAlgorithms::child(node, slot);
        auto *right = NodeAlgorithms::child(node, slot + 1);

        move_value(left->m_values[K_MIN_DEGREE - 1], node->m_values[slot]);
        for (size_type i = 0; i < right->m_count; ++i) {
            move_value(left->m_values[i + K_MIN_DEGREE], right->m_values[i]);
        }

        NodeAlgorithms::move_children_for_merge(left, right);

        left->m_count += right->m_count + 1;

        for (size_type i = slot; i + 1 < node->m_count; ++i) {
            move_value(node->m_values[i], node->m_values[i + 1]);
        }
        NodeAlgorithms::erase_child_after_merge(node, slot);
        --node->m_count;

        destroy_node(right);
        m_impl.m_observer.on_node_merge(*this, node_view(left));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::borrow_from_prev(Node *node, size_type slot) -> void {
        auto *child = NodeAlgorithms::child(node, slot);
        auto *sibling = NodeAlgorithms::child(node, slot - 1);

        for (size_type i = child->m_count; i > 0; --i) {
            move_value(child->m_values[i], child->m_values[i - 1]);
        }
        NodeAlgorithms::move_last_child_to_front(child, sibling);

        move_value(child->m_values[0], node->m_values[slot - 1]);
        move_value(node->m_values[slot - 1], sibling->m_values[sibling->m_count - 1]);
        ++child->m_count;
        --sibling->m_count;
        m_impl.m_observer.on_node_rebalance(*this, node_view(child));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::borrow_from_next(Node *node, size_type slot) -> void {
        auto *child = NodeAlgorithms::child(node, slot);
        auto *sibling = NodeAlgorithms::child(node, slot + 1);

        move_value(child->m_values[child->m_count], node->m_values[slot]);
        NodeAlgorithms::move_first_child_to_back(child, sibling);

        move_value(node->m_values[slot], sibling->m_values[0]);
        for (size_type i = 0; i + 1 < sibling->m_count; ++i) {
            move_value(sibling->m_values[i], sibling->m_values[i + 1]);
        }

        ++child->m_count;
        --sibling->m_count;
        m_impl.m_observer.on_node_rebalance(*this, node_view(child));
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
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
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::erase_from_node(Node *node, key_type const &key) -> bool {
        auto current_key = key;
        auto *current = node;

        while (current != nullptr) {
            auto slot = find_slot(current, current_key);
            if (slot < current->m_count && equal(current_key, key_of(*current->m_values[slot]))) {
                if (NodeAlgorithms::is_leaf(current)) {
                    erase_from_leaf(current, slot);
                    return true;
                }

                auto *left = NodeAlgorithms::child(current, slot);
                auto *right = NodeAlgorithms::child(current, slot + 1);

                if (NodeAlgorithms::has_min_degree(left)) {
                    auto pred = rightmost(this, left);
                    current_key = key_of(*pred);
                    move_value(current->m_values[slot], pred.m_node->m_values[pred.m_slot]);
                    current = left;
                    continue;
                }

                if (NodeAlgorithms::has_min_degree(right)) {
                    auto succ = leftmost(this, right);
                    current_key = key_of(*succ);
                    move_value(current->m_values[slot], succ.m_node->m_values[succ.m_slot]);
                    current = right;
                    continue;
                }

                current_key = key_of(*current->m_values[slot]);
                merge_children(current, slot);
                current = left;
                continue;
            }

            if (NodeAlgorithms::is_leaf(current)) {
                return false;
            }

            auto const was_last = slot == current->m_count;
            if (NodeAlgorithms::needs_fill(NodeAlgorithms::child(current, slot))) {
                fill_child(current, slot);
                if (was_last && slot > current->m_count) {
                    --slot;
                }
            }
            current = NodeAlgorithms::child(current, slot);
        }

        return false;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::erase(key_type const &key) -> size_type {
        if (m_impl.m_root == nullptr) {
            return 0;
        }

        m_impl.m_observer.on_erase(*this, key);
        auto const erased = erase_from_node(m_impl.m_root, key);
        if (!erased) {
            return 0;
        }

        if (m_impl.m_root->m_count == 0) {
            auto *old_root = m_impl.m_root;
            m_impl.m_root = NodeAlgorithms::take_first_child(old_root);
            destroy_node(old_root);
        }
        return 1;
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
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
        return next_key.has_value() ? lower_bound_node(*next_key) : end();
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
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
        return lower_bound_node(last_key);
    }

    template <typename Key, typename Value, typename KeyOfValue, typename Compare, typename Allocator,
              typename Observer, typename Options, std::size_t Order>
    auto BTree<Key, Value, KeyOfValue, Compare, Allocator, Observer, Options, Order>::swap(Self &other) noexcept -> void {
        using std::swap;
        swap(m_impl, other.m_impl);
    }
} // namespace ustl::collections::detail

#endif // #ifndef USTL_COLLECTIONS_DETAIL_BTREE_HPP
