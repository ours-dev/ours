/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_PREEMPTION_STATE_HPP
#define OURS_TASK_PREEMPTION_STATE_HPP 1

#include <ours/assert.hpp>
#include <ours/compiler.hpp>
#include <ours/cpu.hpp>
#include <ours/cpu_mask.hpp>
#include <ours/syscall/time.hpp>
#include <ours/types.hpp>

#include <atomic>

#include <ustl/bitfields.hpp>
#include <ustl/limits.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::task {
/// TODO(SmallHuaZi): From per thread to per scheduler.
class PreemptionState {
    using Self = PreemptionState;

  public:
    PreemptionState() : m_counter(), m_pendings() {
        m_counter.set<Tag::NoReschedEager>(true);
        m_counter.set<Tag::NoReschedPreempt>(true);
    }

    ~PreemptionState() = default;

    /// Enabled iff no disable count is outstanding and no timeslice extension
    /// is in force. Those are not the whole word: it also packs the two
    /// NoResched flags, and the constructor sets both to true, so the old
    /// `!raw_value()` was false for every thread from birth: no
    /// enable_preemption() ever flushed a pending request and every voluntary
    /// reschedule was refused. Those flags record whether a *request* is
    /// pending; they say nothing about whether preemption is *allowed*, and are
    /// left out here on purpose.
    ///
    /// Nor is there an interrupt or NMI term. An interrupt handler is covered
    /// by the two disable counts, which irq::handle_irq_preamble() raises on
    /// the way in; an NMI takes no bookkeeping at all, by design. See the note
    /// on Tag for why the word keeps no nesting count of its own.
    FORCE_INLINE
    auto is_preemption_enabled() const -> bool {
        return preemption_disabled_count() == 0 && eager_resched_disabled_count() == 0 &&
               !m_counter.get<Tag::TimesliceExtension>();
    }

    FORCE_INLINE
    auto enable_eager_reschedule() -> void {
        auto const old_state = m_counter.sub<Tag::EagerReschedDisabledCounter>(1);
        if (old_state > 1) {
            return;
        }

        if (is_preemption_enabled()) {
            flush_pending_local();
        }
        flush_pending_remote();
    }

    FORCE_INLINE
    auto disable_eager_reschedule() -> void {
        m_counter.add<Tag::EagerReschedDisabledCounter>(1);
    }

    FORCE_INLINE
    auto enable_preemption() -> void {
        m_counter.sub<Tag::PreemptionDisabledCounter>(1);
        if (is_preemption_enabled()) {
            flush_pending_local();
        }
    }

    FORCE_INLINE
    auto enable_preemption_delay_resched() -> void {
        DEBUG_ASSERT(preemption_disabled_count() > 0);
        m_counter.sub<Tag::PreemptionDisabledCounter>(1);
    }

    FORCE_INLINE
    auto disable_preemption() -> void {
        DEBUG_ASSERT(preemption_disabled_count() < MAX_COUNT_VALUE);
        m_counter.add<Tag::PreemptionDisabledCounter>(1);
    }

    FORCE_INLINE
    auto add_pending(CpuMask mask = CpuMask::of(Cpu::id())) -> void {
        if (mask.test(Cpu::id())) {
            m_counter.set<Tag::NoReschedPreempt>(false);
        }

        m_pendings.store(m_pendings.load() | mask);
    }

    FORCE_INLINE
    auto clear_pending() -> void {
        m_counter.set<Tag::NoReschedPreempt>(1);
        m_counter.set<Tag::NoReschedEager>(1);
        m_pendings.store(CpuMask());
    }

    FORCE_INLINE
    auto preemption_disabled_count() const -> u8 {
        return m_counter.get<Tag::PreemptionDisabledCounter>();
    }

    FORCE_INLINE
    auto eager_resched_disabled_count() const -> u8 {
        return m_counter.get<Tag::EagerReschedDisabledCounter>();
    }

    FORCE_INLINE
    auto has_preempt_request() const -> bool {
        return !m_counter.get<Tag::NoReschedPreempt>();
    }

    /// Consumes the local CPU's pending preemption: clears its bit in the mask
    /// and the request flag, and says whether there was one. The two live
    /// apart -- add_pending() sets both, and until now only clear_pending()
    /// cleared the flag, wholesale -- so a consumer that cleared the mask bit
    /// alone left has_preempt_request() true forever, and every later
    /// interrupt exit would have preempted again for nothing. This only
    /// consumes the request: the answer goes to the caller, who owns the
    /// preemption.
    [[nodiscard]]
    FORCE_INLINE
    auto take_pending_local() -> bool {
        auto const this_cpu = Cpu::id();
        auto const pending = m_pendings.load();
        if (!pending.test(this_cpu)) {
            return false;
        }
        m_pendings.store(pending & ~CpuMask::of(this_cpu));
        m_counter.set<Tag::NoReschedPreempt>(true);
        return true;
    }

    auto set_timeslice_extension(Duration duration) -> bool;
    auto cancel_timeslice_extension() -> void;

    FORCE_INLINE
    auto ignore_local_pending() -> void {}

    auto flush_pending_remote() -> void;

    constexpr static auto MAX_COUNT_VALUE = ustl::NumericLimits<u8>::max();

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(PreemptionState);

    FORCE_INLINE
    auto raw_value() const -> u32 {
        return m_counter.into<u32>();
    }

    auto flush_pending() -> void;

    auto flush_pending_local() -> void;

    /// The word keeps no interrupt nesting counts. Ours had IrqNestedCounter
    /// and NmiNestedCounter here, both read by is_preemption_enabled() and
    /// neither ever written. They were removed rather than maintained:
    ///
    ///  - Keeping the IRQ count would have changed nothing. The IRQ preamble
    ///    already raises both disable counts, so is_preemption_enabled() is
    ///    false for the whole handler without it. That question needs the
    ///    disable counts and the timeslice extension flag, and nothing more.
    ///  - Nothing here needs what a nesting count is otherwise for: letting
    ///    the outermost interrupt exit know it is the outermost, so that it
    ///    may run deferred interrupt work, and telling interrupt context from
    ///    deferred-work context from task context. Ours runs no deferred work
    ///    on interrupt exit.
    ///  - Ours has one preamble for every vector, #PF included, so a count
    ///    kept there would call a page fault an interrupt unless the preamble
    ///    learned to tell device interrupts from exceptions -- all of it for a
    ///    field nobody reads.
    ///  - The NMI path takes no bookkeeping at all (arch_handle_exception()).
    ///    An NMI lands anywhere, halfway through the IRQ preamble included,
    ///    and must not touch the state the code beneath it is changing. A
    ///    count here could only ever be written from exactly that path.
    ///
    /// Nested interrupt service does not want a count here either. Nesting is
    /// a property of the CPU, not of the thread, and while any handler is on
    /// the stack the thread cannot switch, so a per-thread count would only
    /// mirror per-CPU state. For preemption, the disable counts already nest:
    /// an inner handler's exit sees the outer handler's count still raised,
    /// so only the outermost exit takes a pending preemption. What nesting
    /// will need lives per CPU beside CURRENT_FRAME in kernel/irq: the
    /// preamble saving the frame it replaces and the postamble restoring it
    /// rather than writing null, plus a depth there if something has to ask
    /// how deep it is.
    ///
    /// "Is this CPU handling an interrupt" is answered per CPU by
    /// irq::current_frame(), which the preamble sets and the postamble
    /// clears; logz4 and the sampling profiler read it. A second signal kept
    /// here would only drift from the first, as these did.
    enum class Tag {
        PreemptionDisabledCounter,
        EagerReschedDisabledCounter,

        NoReschedPreempt,
        NoReschedEager,
        TimesliceExtension,
    };

    template <Tag Id, usize Bits, typename Type>
    using Field = ustl::Field<ustl::bitfields::Id<Id>, ustl::bitfields::Bits<Bits>,
                              ustl::bitfields::Type<Type>>;

    using BitField = ustl::BitFields<Field<Tag::PreemptionDisabledCounter, 8, u8>,   // Bits 0..7
                                     Field<Tag::EagerReschedDisabledCounter, 8, u8>, // Bits 8..15
                                     Field<Tag::NoReschedPreempt, 1, bool>,          // Bit 16
                                     Field<Tag::NoReschedEager, 1, bool>,            // Bit 17
                                     Field<Tag::TimesliceExtension, 1, bool>         // Bit 18
                                     >;
    BitField m_counter;

    std::atomic<Duration> m_timeslice_extension;

    /// CPUs whose current threads need a pending reschedule.
    std::atomic<CpuMask> m_pendings;
};
} // namespace ours::task

#endif // #ifndef OURS_TASK_PREEMPTION_STATE_HPP
