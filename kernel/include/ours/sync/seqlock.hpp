/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Sequence locks: for data read far more often than it is written, where
/// readers must never hold a writer up.
///
/// A writer bumps a counter on the way in and again on the way out, leaving
/// it odd for the duration. A reader takes the counter before and after its
/// read and keeps the result only if both readings agree and neither was odd.
/// Readers therefore never write, never block, and never appear in the
/// writer's path at all -- the price being that they may have to read again.
///
/// The API takes the read and the write as functions rather than exposing the
/// counter protocol, because every part of that protocol is load-bearing and
/// none of it is obvious: forget the retry and you keep torn data, forget a
/// barrier and the compiler hoists the read out of the window that was meant
/// to cover it.
///
/// Three rules the type cannot enforce for you:
///
///   1. A read function must have no side effects. It runs on data a writer
///      may be part-way through changing, and only afterwards is that found
///      out and the function run again. Compute and return; do not publish,
///      do not take another lock, do not fault on what you see.
///
///   2. Writers must be serialised against each other. SeqCount does not do
///      it -- two writers overlapping leave the counter even while the data is
///      half written, which no reader can detect. Use SeqCount only where
///      something else already guarantees a single writer; otherwise use
///      SeqLock, which carries the lock.
///
///   3. A writer must not be interruptible by a reader on the same CPU. The
///      reader would spin on the odd counter waiting for a writer that cannot
///      run until the reader returns. SeqLock closes this by keeping
///      interrupts off across the write; a bare SeqCount leaves it to you.
#ifndef OURS_SYNC_SEQLOCK_HPP
#define OURS_SYNC_SEQLOCK_HPP 1

#include <arch/interrupt.hpp>
#include <arch/spin_hint.hpp>
#include <ours/assert.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/types.hpp>

#include <ustl/function/invoke.hpp>
#include <ustl/sync/atomic.hpp>
#include <ustl/traits/is_invocable.hpp>
#include <ustl/traits/is_same.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::sync {
    /// The counter on its own, for callers that already have a single writer.
    class SeqCount {
        using Self = SeqCount;
      public:
        /// 32 bits, not 8. The counter moves twice per write, so a narrow one
        /// wraps in ordinary time -- at 8 bits, after 128 writes -- and a
        /// reader delayed across a wrap reads the same value twice and
        /// concludes, wrongly, that nothing happened. 32 bits needs two
        /// billion writes to come back around.
        using Sequence = u32;

        constexpr SeqCount() : m_sequence(0) {}

        /// Runs |fn| until it sees a snapshot no writer cut into, and returns
        /// what that run produced. |fn| may run several times, and every run
        /// but the last is discarded -- see rule 1 above.
        template <typename F>
            requires ustl::traits::Invocable<F>
        auto read(F &&fn) const -> ustl::traits::InvokeResultT<F> {
            using Result = ustl::traits::InvokeResultT<F>;

            for (;;) {
                auto const start = read_begin();

                if constexpr (ustl::traits::IsSameV<Result, void>) {
                    ustl::function::invoke(fn);
                    if (!read_retry(start)) {
                        return;
                    }
                } else {
                    Result result = ustl::function::invoke(fn);
                    if (!read_retry(start)) {
                        return result;
                    }
                }
            }
        }

        /// Runs |fn| with the counter held odd. Callers must ensure no other
        /// writer is running -- see rule 2 above.
        template <typename F>
            requires ustl::traits::Invocable<F>
        auto write(F &&fn) -> void {
            write_begin();
            ustl::function::invoke(fn);
            write_end();
        }

        /// For assertions. True while a writer sits between begin and end.
        auto is_writing() const -> bool {
            return (m_sequence.load(ustl::sync::MemoryOrder::Relaxed) & 1) != 0;
        }

      private:
        USTL_NO_MOVEABLE_AND_COPYABLE(SeqCount);

        auto read_begin() const -> Sequence {
            for (;;) {
                // Acquire: the read of the data must not be hoisted above this.
                auto const seq = m_sequence.load(ustl::sync::MemoryOrder::Acquire);
                if ((seq & 1) == 0) {
                    return seq;
                }

                // Odd, so a write is in flight and reading now would only be
                // thrown away. Wait on it the way any spin waits.
                arch::spin_hint();
            }
        }

        auto read_retry(Sequence start) const -> bool {
            // The mirror of the acquire above: the read of the data must not
            // sink past this second look at the counter, or the comparison
            // would be covering nothing.
            ustl::sync::atomic_thread_fence(ustl::sync::MemoryOrder::Acquire);
            return m_sequence.load(ustl::sync::MemoryOrder::Relaxed) != start;
        }

        auto write_begin() -> void {
            auto const seq = m_sequence.load(ustl::sync::MemoryOrder::Relaxed);

            // Catches the overlap rule 2 warns about, in the one case where it
            // is catchable: a second writer arriving while the first is inside.
            DEBUG_ASSERT((seq & 1) == 0, "SeqCount: overlapping writers");

            m_sequence.store(seq + 1, ustl::sync::MemoryOrder::Relaxed);
            ustl::sync::atomic_thread_fence(ustl::sync::MemoryOrder::Release);
        }

        auto write_end() -> void {
            // Release before the counter goes even, so that a reader seeing
            // the even value sees every write that came before it.
            ustl::sync::atomic_thread_fence(ustl::sync::MemoryOrder::Release);
            m_sequence.store(m_sequence.load(ustl::sync::MemoryOrder::Relaxed) + 1,
                             ustl::sync::MemoryOrder::Relaxed);
        }

        ustl::sync::Atomic<Sequence> m_sequence;
    };

    /// The counter with the writer's lock attached, which is what most call
    /// sites want: it serialises writers and keeps interrupts off across the
    /// write, so rules 2 and 3 are handled rather than delegated.
    class SeqLock {
        using Self = SeqLock;
      public:
        using Sequence = SeqCount::Sequence;

        SeqLock() = default;

        /// Readers do not take the lock. That is the whole point of the type:
        /// a reader costs two counter loads and never delays a writer.
        template <typename F>
            requires ustl::traits::Invocable<F>
        auto read(F &&fn) const -> ustl::traits::InvokeResultT<F> {
            return m_seq.read(static_cast<F &&>(fn));
        }

        template <typename F>
            requires ustl::traits::Invocable<F>
        auto write(F &&fn) -> void {
            // Interrupts stay off for the whole write, not merely for the
            // lock. A reader taken as an interrupt on this CPU would spin on
            // the odd counter, waiting on a writer that cannot resume until
            // that interrupt returns.
            arch::IrqSavedState state;
            m_lock.acquire_irq_save(state);

            m_seq.write(static_cast<F &&>(fn));

            m_lock.release_irq_restore(state);
        }

        auto is_writing() const -> bool {
            return m_seq.is_writing();
        }

      private:
        USTL_NO_MOVEABLE_AND_COPYABLE(SeqLock);

        SeqCount m_seq;
        SpinLock m_lock;
    };

} // namespace ours::sync

#endif // #ifndef OURS_SYNC_SEQLOCK_HPP
