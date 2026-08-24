/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <array>
#include <span>

#include <ustl/collections/intrusive/unordered_set.hpp>

namespace {
    struct Value final : ustl::collections::intrusive::UnorderedSetBaseHook<> {
        int m_key {};
    };

    struct Hash final {
        [[nodiscard]] constexpr auto operator()(Value const &value) const noexcept -> std::size_t {
            return static_cast<std::size_t>(value.m_key % 3);
        }
    };

    struct Equal final {
        [[nodiscard]] constexpr auto operator()(Value const &left, Value const &right) const noexcept -> bool {
            return left.m_key == right.m_key;
        }
    };

    using Set = ustl::collections::intrusive::UnorderedSet<
        Value,
        ustl::collections::intrusive::Hash<Hash>,
        ustl::collections::intrusive::Equal<Equal>>;
    using Multiset = ustl::collections::intrusive::UnorderedMultiset<
        Value,
        ustl::collections::intrusive::Hash<Hash>,
        ustl::collections::intrusive::Equal<Equal>>;
} // namespace

auto main() -> int {
    Value one {.m_key = 1};
    Value four {.m_key = 4};
    Value duplicate {.m_key = 4};
    Value absent {.m_key = 2};
    std::array<Set::BucketType, 5> buckets {};
    std::array<Set::BucketType, 11> larger_buckets {};
    Set set {std::span {buckets}};
    const auto [one_iterator, inserted_one] = set.insert(one);
    const auto [four_iterator, inserted_four] = set.insert(four);
    const auto [duplicate_iterator, inserted_duplicate] = set.insert(duplicate);
    if (!inserted_one || !inserted_four || inserted_duplicate || one_iterator->m_key != 1 || four_iterator->m_key != 4
        || duplicate_iterator->m_key != 4 || set.size() != 2U || set.bucket_count() != buckets.size()) {
        return 1;
    }

    const auto found = set.find(four);
    if (!found.is_some() || found.value().m_key != 4 || set.find(absent).is_some() || set.count(four) != 1U) {
        return 2;
    }

    set.rehash(std::span {larger_buckets});
    if (set.bucket_count() != larger_buckets.size() || !set.find(one).is_some() || !set.find(four).is_some()) {
        return 3;
    }

    set.erase(four);
    if (set.find(four).is_some() || set.size() != 1U || !set.find(one).is_some()) {
        return 4;
    }

    Value multi_one {.m_key = 7};
    Value multi_two {.m_key = 7};
    std::array<Multiset::BucketType, 3> multiset_buckets {};
    Multiset multiset {std::span {multiset_buckets}};
    static_cast<void>(multiset.insert(multi_one));
    static_cast<void>(multiset.insert(multi_two));
    return multiset.size() == 2U && multiset.count(multi_one) == 2U && multiset.find(multi_one).is_some() ? 0 : 5;
}
