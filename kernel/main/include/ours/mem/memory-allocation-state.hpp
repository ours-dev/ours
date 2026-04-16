/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_MEMORY_ALLOCATION_STATE_HPP
#define OURS_MEM_MEMORY_ALLOCATION_STATE_HPP 1

#include <ours/mem/gaf.hpp>
#include <ours/mem/node-mask.hpp>
#include <ours/mem/priv/memory-allocation-policy.hpp>

#include <ustl/rc.hpp>
#include <ustl/sync/atomic.hpp>

namespace ours::mem {
    class MemoryAllocationState {
        using Self = MemoryAllocationState;
      public:
        auto enable() -> void {
            atomic_signal_fence(ustl::sync::MemoryOrder::SeqCst);
            DEBUG_ASSERT(m_disable_count);
            m_disable_count -= 1;
            atomic_signal_fence(ustl::sync::MemoryOrder::SeqCst);
        }

        auto disable() -> void {
            atomic_signal_fence(ustl::sync::MemoryOrder::SeqCst);
            m_disable_count += 1;
            atomic_signal_fence(ustl::sync::MemoryOrder::SeqCst);
        }

        auto is_enabled() const -> bool {
            atomic_signal_fence(ustl::sync::MemoryOrder::SeqCst);
            return !m_disable_count;
        }

        auto get_policy() -> ustl::Rc<MemoryAllocationPolicy> {
            return m_policy;
        }
      private:
        volatile u16 m_disable_count;
        ustl::Rc<MemoryAllocationPolicy> m_policy;
    };
} // namespace ours::mem

#endif // #ifndef OURS_MEM_MEMORY_ALLOCATION_STATE_HPP