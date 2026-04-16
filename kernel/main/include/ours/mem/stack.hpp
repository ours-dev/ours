/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_STACK_HPP
#define OURS_MEM_STACK_HPP 1

#include <ours/mem/types.hpp>
#include <ours/mem/vm_mapping.hpp>

#include <ustl/util/noncopyable.hpp>

namespace ours::mem {
    class Stack {
        using Self = Stack;
      public:
        CXX11_CONSTEXPR
        static auto const kDefaultStackSize = KB(16); // Four pages.

        Stack();

        ~Stack() = default;

        auto init(usize size = kDefaultStackSize) -> Status;

        auto init_first() -> void;

        FORCE_INLINE
        auto top() const -> VirtAddr {
            return m_base + m_size;
        }
      private:
        USTL_NO_MOVEABLE_AND_COPYABLE(Stack);

        ustl::Rc<VmMapping> m_stack;
        VirtAddr m_base;
        usize m_size;
    };

} // namespace ours::mem

#endif // #ifndef OURS_MEM_STACK_HPP