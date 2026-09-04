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
#include <ustl/bitfields.hpp>
#include <ustl/limits.hpp>
#include <ustl/sync/atomic.hpp>
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
    /// is in force. That is Zircon's PreemptIsEnabled() -- `state_ == 0` --
    /// but Zircon's word holds only those things. Ours also packs the two
    /// NoResched flags into it, and the constructor sets both to true, so the
    /// old `!raw_value()` was false for every thread from birth: no
    /// enable_preemption() ever flushed a pending request and every voluntary
    /// reschedule was refused. Those flags record whether a *request* is
    /// pending; they say nothing about whether preemption is *allowed*, and
    /// are left out here on purpose.
    FORCE_INLINE auto is_preemption_enabled() const -> bool {
        return preemption_disabled_count() == 0 && eager_resched_disabled_count() == 0 &&
               !in_interrupt() && !m_counter.get<Tag::NmiNestedCounter>() &&
               !m_counter.get<Tag::TimesliceExtension>();
    }

    FORCE_INLINE auto enable_eager_reschedule() -> void {
        auto const old_state = m_counter.sub<Tag::EagerReschedDisabledCounter>(1);
        if (old_state > 1) {
            return;
        }

        if (is_preemption_enabled()) {
            flush_pending_local();
        }
        flush_pending_remote();
    }

    FORCE_INLINE auto disable_eager_reschedule() -> void {
        m_counter.add<Tag::EagerReschedDisabledCounter>(1);
    }

    FORCE_INLINE auto enable_preemption() -> void {
        m_counter.sub<Tag::PreemptionDisabledCounter>(1);
        if (is_preemption_enabled()) {
            flush_pending_local();
        }
    }

    FORCE_INLINE auto enable_preemption_delay_resched() -> void {
        DEBUG_ASSERT(preemption_disabled_count() > 0);
        m_counter.sub<Tag::PreemptionDisabledCounter>(1);
    }

    FORCE_INLINE auto disable_preemption() -> void {
        DEBUG_ASSERT(preemption_disabled_count() < MAX_COUNT_VALUE);
        m_counter.add<Tag::PreemptionDisabledCounter>(1);
    }

    FORCE_INLINE auto add_pending(CpuMask mask = CpuMask::of(Cpu::id())) -> void {
        if (mask.test(Cpu::id())) {
            m_counter.set<Tag::NoReschedPreempt>(false);
        }

        m_pendings.store(m_pendings.load() | mask);
    }

    FORCE_INLINE auto clear_pending() -> void {
        m_counter.set<Tag::NoReschedPreempt>(1);
        m_counter.set<Tag::NoReschedEager>(1);
        m_pendings.store(CpuMask());
    }

    FORCE_INLINE auto in_interrupt() const -> bool {
        return m_counter.get<Tag::IrqNestedCounter>();
    }

    FORCE_INLINE auto preemption_disabled_count() const -> u8 {
        return m_counter.get<Tag::PreemptionDisabledCounter>();
    }

    FORCE_INLINE auto eager_resched_disabled_count() const -> u8 {
        return m_counter.get<Tag::EagerReschedDisabledCounter>();
    }

    FORCE_INLINE auto has_preempt_request() const -> bool {
        return !m_counter.get<Tag::NoReschedPreempt>();
    }

    /// Consumes the local CPU's pending preemption: clears its bit in the mask
    /// and the request flag, and says whether there was one. The two live
    /// apart -- add_pending() sets both, and until now only clear_pending()
    /// cleared the flag, wholesale -- so a consumer that cleared the mask bit
    /// alone left has_preempt_request() true forever, and every later
    /// interrupt exit would have preempted again for nothing. Zircon's
    /// PreemptReenableDelayFlush() does the same fetch_and on its pending
    /// mask and hands the result to the caller, who owns the preemption.
    [[nodiscard]]
    FORCE_INLINE auto take_pending_local() -> bool {
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

    FORCE_INLINE auto ignore_local_pending() -> void {}

    auto flush_pending_remote() -> void;

    constexpr static auto MAX_COUNT_VALUE = ustl::NumericLimits<u8>::max();

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(PreemptionState);

    FORCE_INLINE auto raw_value() const -> u32 {
        return m_counter.into<u32>();
    }

    auto flush_pending() -> void;

    auto flush_pending_local() -> void;

    enum class Tag {
        PreemptionDisabledCounter,
        EagerReschedDisabledCounter,
        IrqNestedCounter,
        NmiNestedCounter,

        NoReschedPreempt,
        NoReschedEager,
        TimesliceExtension,
    };

    template <Tag Id, usize Bits, typename Type>
    using Field = ustl::Field<ustl::bitfields::Id<Id>, ustl::bitfields::Bits<Bits>,
                              ustl::bitfields::Type<Type>>;

    using BitField = ustl::BitFields<Field<Tag::PreemptionDisabledCounter, 8, u8>,   // Bits 0..7
                                     Field<Tag::EagerReschedDisabledCounter, 8, u8>, // Bits 8..15
                                     Field<Tag::IrqNestedCounter, 4, u8>,            // Bits 16..19
                                     Field<Tag::NmiNestedCounter, 4, u8>,            // Bits 20..23
                                     Field<Tag::NoReschedPreempt, 1, bool>,          // Bit 24
                                     Field<Tag::NoReschedEager, 1, bool>,            // Bit 25
                                     Field<Tag::TimesliceExtension, 1, bool>         // Bit 25
                                     >;
    BitField m_counter;

    ustl::sync::Atomic<Duration> m_timeslice_extension;

    /// CPUs whose current threads need a pending reschedule.
    ustl::sync::Atomic<CpuMask> m_pendings;
};
} // namespace ours::task

#endif // #ifndef OURS_TASK_PREEMPTION_STATE_HPP
