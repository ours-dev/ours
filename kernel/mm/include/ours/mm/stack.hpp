/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_STACK_HPP
#define OURS_MM_STACK_HPP 1

#include <ours/mm/types.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::mm {
class Stack {
    using Self = Stack;

  public:
    constexpr static auto const DEFAULT_STACK_SIZE = KB(16); // Four pages.

    Stack();

    ~Stack() = default;

    auto init(usize size = DEFAULT_STACK_SIZE) -> Status;

    auto init_first() -> void;

    FORCE_INLINE auto top() const -> VirtAddr {
        return m_base + m_size;
    }

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(Stack);

    ustl::Arc<VmMapping> m_stack;
    VirtAddr m_base;
    usize m_size;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_STACK_HPP