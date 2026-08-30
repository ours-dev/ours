/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_BUFFER_HPP
#define PROF_BUFFER_HPP 1

#include <prof/format.hpp>
#include <ustl/sync/atomic.hpp>

#include <cstddef>
#include <cstdint>

namespace prof {

/// Per-CPU sample arena. The sampling interrupt is the only producer on a
/// CPU (interrupts are disabled within it), so a single atomic cursor gives
/// a wait-free produce path; the drain side only runs after sampling has
/// quiesced, which rules out torn records by protocol rather than locking.
class SampleBuffer final {
  public:
    static constexpr std::size_t CAPACITY = 128;

    auto reset() -> void {
        m_write_index.store(0, ustl::sync::MemoryOrder::Relaxed);
        m_dropped.store(0, ustl::sync::MemoryOrder::Relaxed);
        m_total.store(0, ustl::sync::MemoryOrder::Relaxed);
    }

    /// Record one sample; returns false (and counts a drop) when full.
    auto try_produce(SampleRecord const &record) -> bool {
        m_total.fetch_add(1, ustl::sync::MemoryOrder::Relaxed);
        std::size_t const slot = m_write_index.fetch_add(1, ustl::sync::MemoryOrder::Relaxed);
        if (slot >= CAPACITY) {
            m_dropped.fetch_add(1, ustl::sync::MemoryOrder::Relaxed);
            return false;
        }
        m_slots[slot] = record;
        return true;
    }

    /// Number of retained records; stable once sampling has stopped.
    [[nodiscard]]
    auto size() const -> std::size_t {
        auto const written = m_write_index.load(ustl::sync::MemoryOrder::Relaxed);
        if (written < CAPACITY) {
            return written;
        }
        return CAPACITY;
    }

    [[nodiscard]]
    auto dropped() const -> std::uint64_t {
        return m_dropped.load(ustl::sync::MemoryOrder::Relaxed);
    }

    [[nodiscard]]
    auto total() const -> std::uint64_t {
        return m_total.load(ustl::sync::MemoryOrder::Relaxed);
    }

    [[nodiscard]]
    auto slot(std::size_t index) const -> SampleRecord const & {
        return m_slots[index];
    }

  private:
    ustl::sync::Atomic<std::size_t> m_write_index{0};
    ustl::sync::Atomic<std::uint64_t> m_dropped{0};
    ustl::sync::Atomic<std::uint64_t> m_total{0};
    SampleRecord m_slots[CAPACITY] = {};
};

} // namespace prof

#endif // #ifndef PROF_BUFFER_HPP
