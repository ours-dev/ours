/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <array>
#include <cstddef>

#include <ustl/collections/intrusive/splay_set.hpp>

namespace {
    struct Value final : ustl::collections::intrusive::SplaySetBaseHook<> {
        int m_key {};
    };

    struct Compare final {
        [[nodiscard]] constexpr auto operator()(Value const &left, Value const &right) const noexcept -> bool {
            return left.m_key < right.m_key;
        }
    };

    using Splaytree = ustl::collections::intrusive::Splaytree<
        Value,
        ustl::collections::intrusive::Compare<Compare>>;
    using NodeTraits = Value::NodeTraits;
    using Algorithms = ustl::collections::intrusive::SplaytreeAlgorithms<NodeTraits>;
} // namespace

auto main() -> int {
    std::array<Value, 7U> values {};
    Splaytree tree {};
    for (std::size_t index = 0U; index < values.size(); ++index) {
        values[index].m_key = static_cast<int>(index);
        const auto [iterator, inserted] = tree.insert_unique(values[index]);
        if (!inserted || iterator->m_key != static_cast<int>(index)) {
            return 1;
        }
    }

    const auto found = tree.find(values[1]);
    if (!found || &found.value() != &values[1]) {
        return 2;
    }
    auto header = values[1].this_ptr();
    while (!Algorithms::is_header(header)) {
        header = NodeTraits::get_parent(header);
    }
    if (NodeTraits::get_parent(header) != values[1].this_ptr()) {
        return 3;
    }

    tree.erase(values[1]);
    return !tree.find(values[1]) && tree.size() == 6U ? 0 : 4;
}
