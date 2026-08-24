/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/treap_set.hpp>

namespace {
    using Hook = ustl::collections::intrusive::BsSetBaseHook<>;
    using Node = Hook::NodeTraits::Node;
    using NodePtr = Hook::NodeTraits::NodePtr;
    using NodeTraits = Hook::NodeTraits;

    struct Value final : Hook {
        int m_key {};
        int m_priority {};
    };

    struct Compare final {
        [[nodiscard]] constexpr auto operator()(Value const &left, Value const &right) const noexcept -> bool {
            return left.m_key < right.m_key;
        }
    };

    struct PriorityOfValue final {
        using Type = int;

        [[nodiscard]] constexpr auto operator()(Value const &value) const noexcept -> int {
            return value.m_priority;
        }
    };

    using Treap = ustl::collections::intrusive::Treap<
        Value,
        ustl::collections::intrusive::Compare<Compare>,
        ustl::collections::intrusive::PriorityOfValue<PriorityOfValue>>;
    using TreapSet = ustl::collections::intrusive::TreapSet<
        Value,
        ustl::collections::intrusive::Compare<Compare>,
        ustl::collections::intrusive::PriorityOfValue<PriorityOfValue>>;
    using TreapMultiset = ustl::collections::intrusive::TreapMultiset<
        Value,
        ustl::collections::intrusive::Compare<Compare>,
        ustl::collections::intrusive::PriorityOfValue<PriorityOfValue>>;

    [[nodiscard]] constexpr auto to_value(NodePtr const node) noexcept -> Value * {
        auto *const hook = static_cast<Hook *>(static_cast<Node *>(node));
        return static_cast<Value *>(hook);
    }

    [[nodiscard]] constexpr auto has_valid_heap(NodePtr const node) noexcept -> bool {
        if (node == nullptr) {
            return true;
        }

        const auto left = NodeTraits::get_left(node);
        const auto right = NodeTraits::get_right(node);
        const auto value = to_value(node);
        return (left == nullptr || value->m_priority <= to_value(left)->m_priority)
            && (right == nullptr || value->m_priority <= to_value(right)->m_priority) && has_valid_heap(left)
            && has_valid_heap(right);
    }

    [[nodiscard]] constexpr auto has_sorted_iteration(Treap const &tree) noexcept -> bool {
        int previous {};
        bool is_first = true;
        for (auto iterator = tree.begin(); iterator != tree.end(); ++iterator) {
            if (!is_first && iterator->m_key <= previous) {
                return false;
            }
            previous = iterator->m_key;
            is_first = false;
        }
        return true;
    }
} // namespace

auto main() -> int {
    Treap empty {};
    if (empty.top() != empty.end()) {
        return 1;
    }

    Value four {.m_key = 4, .m_priority = 40};
    Value two {.m_key = 2, .m_priority = 20};
    Value six {.m_key = 6, .m_priority = 60};
    Value one {.m_key = 1, .m_priority = 10};
    Value three {.m_key = 3, .m_priority = 30};
    Value five {.m_key = 5, .m_priority = 50};
    Value seven {.m_key = 7, .m_priority = 70};
    Treap tree {};

    const auto [four_it, inserted_four] = tree.insert_unique(four);
    const auto [two_it, inserted_two] = tree.insert_unique(two);
    const auto [six_it, inserted_six] = tree.insert_unique(six);
    const auto [one_it, inserted_one] = tree.insert_unique(one);
    const auto [three_it, inserted_three] = tree.insert_unique(three);
    const auto [five_it, inserted_five] = tree.insert_unique(five);
    const auto [seven_it, inserted_seven] = tree.insert_unique(seven);
    if (!inserted_four || !inserted_two || !inserted_six || !inserted_one || !inserted_three || !inserted_five
        || !inserted_seven || four_it->m_key != 4 || two_it->m_key != 2 || six_it->m_key != 6
        || three_it->m_key != 3 || five_it->m_key != 5 || seven_it->m_key != 7 || one_it->m_key != 1) {
        return 2;
    }
    if (tree.top()->m_key != 1 || !has_sorted_iteration(tree)
        || !has_valid_heap(static_cast<Hook &>(*tree.top()).this_ptr())) {
        return 3;
    }

    tree.erase(one);
    if (tree.top()->m_key != 2 || !has_sorted_iteration(tree)
        || !has_valid_heap(static_cast<Hook &>(*tree.top()).this_ptr())) {
        return 4;
    }

    Value set_value {.m_key = 8, .m_priority = 8};
    Value set_duplicate {.m_key = 8, .m_priority = 1};
    TreapSet set {};
    const auto [set_iterator, inserted_set] = set.insert(set_value);
    const auto [duplicate_iterator, inserted_duplicate] = set.insert(set_duplicate);
    if (!inserted_set || inserted_duplicate || set_iterator->m_key != 8 || duplicate_iterator->m_key != 8) {
        return 5;
    }

    Value multi_one {.m_key = 9, .m_priority = 9};
    Value multi_two {.m_key = 9, .m_priority = 3};
    TreapMultiset multiset {};
    static_cast<void>(multiset.insert(multi_one));
    static_cast<void>(multiset.insert(multi_two));
    return multiset.count(multi_one) == 2U && multiset.top()->m_priority == 3 ? 0 : 6;
}
