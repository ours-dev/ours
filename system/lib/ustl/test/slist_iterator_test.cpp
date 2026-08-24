/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/detail/slist_iterator.hpp>
#include <ustl/collections/intrusive/detail/slist_node.hpp>

namespace {
    struct Item final : ustl::collections::intrusive::SlistNode<void *> {
        int m_value {};
    };

    struct StaticValueTraits final {
        using NodeTraits = ustl::collections::intrusive::SlistNodeTraits<void *>;
        using NodePtr = typename NodeTraits::NodePtr;
        using Element = Item;
        using PtrMut = Item *;
        using Ptr = Item const *;

        static constexpr bool IS_STATEFUL_VALUE_TRAITS = false;

        [[nodiscard]] static constexpr auto to_value_ptr(NodePtr const node) noexcept -> PtrMut {
            return static_cast<PtrMut>(node);
        }
    };

    using IterMut = ustl::collections::intrusive::detail::SlistIter<StaticValueTraits, false>;
    using Iter = ustl::collections::intrusive::detail::SlistIter<StaticValueTraits, true>;
} // namespace

auto main() -> int {
    Item first{};
    Item second{};
    first.m_value = 1;
    second.m_value = 2;
    StaticValueTraits::NodeTraits::set_next(&first, &second);
    StaticValueTraits::NodeTraits::set_next(&second, nullptr);

    IterMut iterator(&first);
    if (iterator.pointed_node() != &first || (*iterator).m_value != 1 || iterator->m_value != 1) {
        return 1;
    }

    const auto previous = iterator++;
    if (previous.pointed_node() != &first || iterator.pointed_node() != &second || (*iterator).m_value != 2) {
        return 2;
    }

    const auto &incremented = ++iterator;
    if (&incremented != &iterator || iterator.pointed_node() != nullptr) {
        return 3;
    }

    Iter const_iterator(previous);
    if (const_iterator.pointed_node() != &first || const_iterator->m_value != 1 || const_iterator.unconst() != previous) {
        return 4;
    }
    return Iter{} == Iter{} && IterMut{} != previous ? 0 : 5;
}
