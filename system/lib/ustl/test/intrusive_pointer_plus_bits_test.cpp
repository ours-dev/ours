/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/intrusive/pointer_plus_bits.hpp>

#include <concepts>

namespace {
struct alignas(8) Node final {
    int m_value = 0;
};

using TaggedPointer = ustl::collections::intrusive::PointerPlusBits<Node *, 2>;
} // namespace

static_assert(ustl::collections::intrusive::MaxPointerPlusBits<void *, alignof(Node)>::VALUE >= 3);
static_assert(ustl::collections::intrusive::MaxPointerPlusBits<int, alignof(Node)>::VALUE == 0);

auto main() -> int {
    Node first{};
    Node second{};
    Node *tagged = nullptr;
    TaggedPointer::set_pointer(tagged, &first);
    TaggedPointer::set_bits(tagged, 3);
    if (TaggedPointer::get_pointer(tagged) != &first || TaggedPointer::get_bits(tagged) != 3) {
        return 1;
    }

    TaggedPointer::set_pointer(tagged, &second);
    return TaggedPointer::get_pointer(tagged) == &second && TaggedPointer::get_bits(tagged) == 3
               ? 0
               : 2;
}
