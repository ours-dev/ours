/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_RECORD_BUFFER_HPP
#define PROF_RECORD_BUFFER_HPP 1

#include <atomic>

namespace prof {

/// How much a single CPU's buffer holds and lost.
struct BufferStats final {
    usize retained = 0;
    u64 dropped = 0;
    u64 total = 0;
};

/// Per-CPU arena of fixed-size records.
///
/// One CPU has exactly one producer -- the interrupt or the traced code path
/// running on it, with interrupts disabled -- so a single atomic cursor gives
/// a wait-free produce path with no locking. The drain side only runs after
/// production has been quiesced, which is what rules out torn records: it is
/// a protocol, not a lock.
///
/// The cursor is also the only atomic, and the only counter. Every produce
/// advances it exactly once whether or not the record fit, so it *is* the
/// total; what was retained is the part of it below `CAPACITY` and what was
/// dropped is the part above. Separate `total`/`dropped` counters could
/// only ever agree with it and would be two more words the interrupt path
/// has to touch for nothing.
///
/// The record type is a parameter because the buffer is deliberately agnostic
/// about what is being recorded. Statistical samples and traced events differ
/// in how they are *triggered*, not in how they are stored, so they share
/// this.
template <typename Record, usize Capacity>
class RecordBuffer final {
  public:
    using Element = Record;
    static constexpr usize CAPACITY = Capacity;

    auto reset() -> void {
        m_write_index.store(0, std::memory_order_relaxed);
    }

    /// Record one entry; returns false when full. The cursor still advances
    /// on a full buffer -- that is how the drop gets counted.
    auto try_produce(Record const &record) -> bool {
        usize const slot = m_write_index.fetch_add(1, std::memory_order_relaxed);
        if (slot >= CAPACITY) {
            return false;
        }
        m_slots[slot] = record;
        return true;
    }

    /// Number of retained records; stable once production has stopped.
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
        auto const written = m_write_index.load(std::memory_order_relaxed);
        if (written < CAPACITY) {
            return 0;
        }
        return written - CAPACITY;
    }

    [[nodiscard]]
    auto total() const -> u64 {
        return m_write_index.load(std::memory_order_relaxed);
    }

    [[nodiscard]]
    auto slot(usize index) const -> Record const & {
        return m_slots[index];
    }

    [[nodiscard]]
    auto stats() const -> BufferStats {
        return BufferStats{.retained = size(), .dropped = dropped(), .total = total()};
    }

  private:
    std::atomic<usize> m_write_index{0};
    Record m_slots[CAPACITY] = {};
};

} // namespace prof

#endif // #ifndef PROF_RECORD_BUFFER_HPP
