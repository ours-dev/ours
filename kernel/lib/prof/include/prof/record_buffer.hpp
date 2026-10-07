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
    /// Events the producer saw and chose not to record (note_skipped()).
    u64 skipped = 0;
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
/// total; what was retained is the part of it below the capacity and what
/// was dropped is the part above. Separate `total`/`dropped` counters could
/// only ever agree with it and would be two more words the interrupt path
/// has to touch for nothing.
///
/// The slots are the caller's, handed in by attach() with their count, so
/// the depth is chosen when the buffer is put to use rather than when the
/// program is compiled: a profiling session sizes its buffers from what its
/// creator asked for. The buffer only borrows them; whoever attached them
/// keeps them alive, and unused, while the buffer can still be produced into
/// or read.
///
/// The record type is a parameter because the buffer is deliberately agnostic
/// about what is being recorded. Statistical samples and traced events differ
/// in how they are *triggered*, not in how they are stored, so they share
/// this.
template <typename Record>
class RecordBuffer final {
  public:
    using Element = Record;

    /// Gives the buffer |capacity| slots at |slots| and empties it. Not
    /// concurrent with production: called before the producer can run, as
    /// reset() is.
    auto attach(Record *slots, usize capacity) -> void {
        m_slots = slots;
        m_capacity = capacity;
        reset();
    }

    auto reset() -> void {
        m_write_index.store(0, std::memory_order_relaxed);
        m_skipped.store(0, std::memory_order_relaxed);
    }

    /// Counts an event the producer saw and did not record, so that a reader
    /// can tell how many there were in all. The same single producer as
    /// try_produce(), so a relaxed counter is enough.
    auto note_skipped() -> void {
        m_skipped.fetch_add(1, std::memory_order_relaxed);
    }

    /// How many records the buffer can retain; 0 until attach().
    [[nodiscard]]
    auto capacity() const -> usize {
        return m_capacity;
    }

    /// Record one entry; returns false when full. The cursor still advances
    /// on a full buffer -- that is how the drop gets counted -- and a buffer
    /// with no slots attached is simply full.
    auto try_produce(Record const &record) -> bool {
        usize const slot = m_write_index.fetch_add(1, std::memory_order_relaxed);
        if (slot >= m_capacity) {
            return false;
        }
        m_slots[slot] = record;
        return true;
    }

    /// Number of retained records; stable once production has stopped.
    [[nodiscard]]
    auto size() const -> usize {
        auto const written = m_write_index.load(std::memory_order_relaxed);
        if (written < m_capacity) {
            return written;
        }
        return m_capacity;
    }

    [[nodiscard]]
    auto dropped() const -> u64 {
        auto const written = m_write_index.load(std::memory_order_relaxed);
        if (written < m_capacity) {
            return 0;
        }
        return written - m_capacity;
    }

    [[nodiscard]]
    auto total() const -> u64 {
        return m_write_index.load(std::memory_order_relaxed);
    }

    /// Visits every retained record, oldest first.
    ///
    /// The buffer walks itself rather than handing out an indexable accessor.
    /// The bound is the buffer's own business: a caller that asks for `size()`
    /// and then indexes has to keep the two in step every time it is written,
    /// and getting it wrong reads a slot that was never produced -- or, past
    /// the capacity, whatever follows the slots in memory. There is no
    /// assertion here because there is no longer anything to assert.
    ///
    /// Valid once production has been quiesced; see the note on the class.
    template <typename Visitor>
    auto for_each_retained(Visitor &&visit) const -> void {
        auto const count = size();
        for (usize i = 0; i < count; ++i) {
            visit(m_slots[i]);
        }
    }

    [[nodiscard]]
    auto stats() const -> BufferStats {
        return BufferStats{.retained = size(),
                           .dropped = dropped(),
                           .total = total(),
                           .skipped = m_skipped.load(std::memory_order_relaxed)};
    }

  private:
    std::atomic<usize> m_write_index{0};
    std::atomic<u64> m_skipped{0};
    Record *m_slots = nullptr;
    usize m_capacity = 0;
};

} // namespace prof

#endif // #ifndef PROF_RECORD_BUFFER_HPP
