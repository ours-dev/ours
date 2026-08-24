/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_MEMORY_ALLOCATION_STATE_HPP
#define OURS_MM_MEMORY_ALLOCATION_STATE_HPP 1

#include <ours/mm/gaf.hpp>
#include <ours/mm/node-mask.hpp>
#include <ours/mm/priv/memory-allocation-policy.hpp>

#include <ustl/rc.hpp>
#include <ustl/sync/atomic.hpp>

namespace ours::mm {
    class MemoryAllocationState {
        using Self = MemoryAllocationState;
      public:
        auto enable() -> void {
            ustl::sync::atomic_signal_fence(ustl::sync::MemoryOrder::SeqCst);
            DEBUG_ASSERT(m_disable_count);
            m_disable_count -= 1;
            ustl::sync::atomic_signal_fence(ustl::sync::MemoryOrder::SeqCst);
        }

        auto disable() -> void {
            ustl::sync::atomic_signal_fence(ustl::sync::MemoryOrder::SeqCst);
            m_disable_count += 1;
            ustl::sync::atomic_signal_fence(ustl::sync::MemoryOrder::SeqCst);
        }

        auto is_enabled() const -> bool {
            ustl::sync::atomic_signal_fence(ustl::sync::MemoryOrder::SeqCst);
            return !m_disable_count;
        }

        auto get_policy() -> ustl::Arc<MemoryAllocationPolicy> {
            return m_policy;
        }
      private:
        volatile u16 m_disable_count = 0;
        ustl::Arc<MemoryAllocationPolicy> m_policy;
    };
} // namespace ours::mm

#endif // #ifndef OURS_MM_MEMORY_ALLOCATION_STATE_HPP
