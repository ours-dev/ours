/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_BUFFER_HPP
#define PROF_BUFFER_HPP 1

#include <prof/format.hpp>

#include <atomic>

namespace prof {

/// Per-CPU sample arena. The sampling interrupt is the only producer on a
/// CPU (interrupts are disabled within it), so a single atomic cursor gives
/// a wait-free produce path; the drain side only runs after sampling has
/// quiesced, which rules out torn records by protocol rather than locking.
class SampleBuffer final {
  public:
    static constexpr usize CAPACITY = 128;

    auto reset() -> void {
        m_write_index.store(0, std::memory_order_relaxed);
        m_dropped.store(0, std::memory_order_relaxed);
        m_total.store(0, std::memory_order_relaxed);
    }

    /// Record one sample; returns false (and counts a drop) when full.
    auto try_produce(SampleRecord const &record) -> bool {
        m_total.fetch_add(1, std::memory_order_relaxed);
        usize const slot = m_write_index.fetch_add(1, std::memory_order_relaxed);
        if (slot >= CAPACITY) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_slots[slot] = record;
        return true;
    }

    /// Number of retained records; stable once sampling has stopped.
    [[nodiscard]]
    auto size() const -> usize {
        auto const written = m_write_index.load(std::memory_order_relaxed);
        if (written < CAPACITY) {
            return written;
        }
        return CAPACITY;
    }

    [[nodiscard]]
    auto dropped() const -> u64 {
        return m_dropped.load(std::memory_order_relaxed);
    }

    [[nodiscard]]
    auto total() const -> u64 {
        return m_total.load(std::memory_order_relaxed);
    }

    [[nodiscard]]
    auto slot(usize index) const -> SampleRecord const & {
        return m_slots[index];
    }

  private:
    std::atomic<usize> m_write_index{0};
    std::atomic<u64> m_dropped{0};
    std::atomic<u64> m_total{0};
    SampleRecord m_slots[CAPACITY] = {};
};

} // namespace prof

#endif // #ifndef PROF_BUFFER_HPP
