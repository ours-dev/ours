/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/bs_set.hpp>

namespace {
    struct Value final : ustl::collections::intrusive::BsSetBaseHook<> {
        int m_key {};
    };

    struct Compare final {
        [[nodiscard]] constexpr auto operator()(Value const &left, Value const &right) const noexcept -> bool {
            return left.m_key < right.m_key;
        }
    };

    using Bstree = ustl::collections::intrusive::Bstree<
        Value,
        ustl::collections::intrusive::Compare<Compare>>;
    using BsSet = ustl::collections::intrusive::BsSet<
        Value,
        ustl::collections::intrusive::Compare<Compare>>;
    using BsMultiset = ustl::collections::intrusive::BsMultiset<
        Value,
        ustl::collections::intrusive::Compare<Compare>>;
} // namespace

auto main() -> int {
    Value one {.m_key = 1};
    Value two {.m_key = 2};
    Value duplicate {.m_key = 2};
    Bstree tree {};
    const auto [one_it, inserted_one] = tree.insert_unique(one);
    const auto [two_it, inserted_two] = tree.insert_unique(two);
    const auto [duplicate_it, inserted_duplicate] = tree.insert_unique(duplicate);
    if (!inserted_one || !inserted_two || inserted_duplicate || one_it->m_key != 1 || two_it->m_key != 2 ||
        duplicate_it->m_key != 2) {
        return 1;
    }
    static_cast<void>(tree.insert_equal(duplicate));
    if (tree.count(two) != 2U) {
        return 2;
    }
    tree.erase(duplicate);

    Value set_value {.m_key = 3};
    Value set_duplicate {.m_key = 3};
    BsSet set {};
    const auto [set_it, inserted_set] = set.insert(set_value);
    const auto [duplicate_it_set, inserted_duplicate_set] = set.insert(set_duplicate);
    if (!inserted_set || inserted_duplicate_set || set_it->m_key != 3 || duplicate_it_set->m_key != 3) {
        return 3;
    }

    Value multi_one {.m_key = 4};
    Value multi_two {.m_key = 4};
    BsMultiset multiset {};
    static_cast<void>(multiset.insert(multi_one));
    static_cast<void>(multiset.insert(multi_two));
    return tree.count(two) == 1U && multiset.count(multi_one) == 2U ? 0 : 4;
}
