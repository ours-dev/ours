/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_MEMORY_ALLOCATION_STATE_HPP
#define OURS_MM_MEMORY_ALLOCATION_STATE_HPP 1

#include <ours/mm/gaf.hpp>
#include <ours/mm/priv/memory_allocation_policy.hpp>
#include <ours/node_mask.hpp>

#include <atomic>

#include <ustl/rc.hpp>

namespace ours::mm {
class MemoryAllocationState {
    using Self = MemoryAllocationState;

  public:
    auto enable() -> void {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        DEBUG_ASSERT(m_disable_count);
        m_disable_count -= 1;
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    auto disable() -> void {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        m_disable_count += 1;
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    auto is_enabled() const -> bool {
        std::atomic_signal_fence(std::memory_order_seq_cst);
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
