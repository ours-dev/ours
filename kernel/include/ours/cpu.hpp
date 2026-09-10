/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_CPU_HPP
#define OURS_CPU_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/types.hpp>

#include <array>
#include <atomic>

namespace ours {

class Cpu {
  public:
    enum class State : usize {
        Possible,
        Present,
        Online,
        Count,
    };

    static auto id() -> CpuNum;

    /// For the node a CPU belongs to, use mm::PmNode::Current::nid(). `Cpu::nid()`
    /// used to duplicate it from a function-local `CPU_LOCAL static` that could
    /// never be written, so it always answered 0; it had no callers.

    /// Acquire, and the counterpart of the release in set_state(): a CPU that
    /// reads this bit set must also see everything the CPU that set it did
    /// beforehand -- its per-CPU area, its idle thread, its scheduler being
    /// marked active. x86_bring_aps() spins on exactly this before it starts
    /// the next CPU.
    ///
    /// The atomic is not only about that ordering. It was a plain BitSet read
    /// in a `while (!Cpu::is_online(cpu));` loop with an empty body, and
    /// nothing in the language stopped the compiler hoisting that load out of
    /// the loop and spinning forever on a stale word. The loop happened to
    /// come out right; whether it did was up to the optimiser, not up to the
    /// code. It is up to the code now -- x86_bring_aps() disassembles with the
    /// load inside the loop:
    ///
    ///     movq  Cpu::ONLINE(%rip), %rcx
    ///     testq %rax, %rcx
    ///     je    .-8                     # back to the load, not past it
    [[nodiscard]]
    static auto is_online(CpuNum cpu) -> bool {
        return ONLINE.load(std::memory_order_acquire).test(cpu);
    }

    static auto set_state(CpuNum cpu, State state, bool enabled = true) -> void {
        if (state != State::Online) {
            immutable_mask(state).set(cpu, enabled);
            return;
        }

        // A CAS loop rather than load/set/store, for the reason
        // Scheduler::set_state() spells out about its own mask: two CPUs
        // flipping different bits at the same time would each store a word
        // missing the other's. Nothing does that today -- x86_bring_aps()
        // starts one CPU at a time -- but "safe because the caller happens to
        // be serialised" is what the rest of this bring-up kept getting wrong,
        // and a parallel bring-up is already on the repair list.
        //
        // Release: everything this CPU did before joining has to be visible to
        // whoever observes the bit. See is_online().
        auto expected = ONLINE.load(std::memory_order_relaxed);
        for (;;) {
            auto desired = expected;
            desired.set(cpu, enabled);
            if (ONLINE.compare_exchange_weak(expected, desired, std::memory_order_release,
                                             std::memory_order_relaxed)) {
                return;
            }
        }
    }

    static auto set_state(State state, bool enabled = true) -> void {
        set_state(id(), state, enabled);
    }

    /// Possible and Present are written once, by the boot CPU, while it parses
    /// the MADT, and never again -- so they are plain masks and are handed out
    /// by reference, which is what lets srat.cpp still narrow the possible set
    /// in place and what ClockEventDevice points at for "any CPU".
    ///
    /// Online is the only one of the three that changes after that, and the
    /// only one another CPU ever writes; it is the atomic below and comes back
    /// by value.
    [[nodiscard]]
    static auto possible_mask() -> CpuMask & {
        return immutable_mask(State::Possible);
    }

    [[nodiscard]]
    static auto present_mask() -> CpuMask & {
        return immutable_mask(State::Present);
    }

    /// A snapshot. The set may have grown by the time the caller looks at it,
    /// which is true of anything read out of a mask other CPUs are joining.
    [[nodiscard]]
    static auto online_mask() -> CpuMask {
        return ONLINE.load(std::memory_order_acquire);
    }

    /// The Online mask itself, for the one caller that must keep following it
    /// rather than take a copy: a thread whose hard affinity is "wherever is
    /// online" has to widen as the secondaries come up, so
    /// CpuAffinity::set_affinity_hard_shared() aims at this cell and loads it
    /// on every query. Everyone else wants online_mask() above.
    [[nodiscard]]
    static auto online_mask_live() -> std::atomic<CpuMask> const & {
        return ONLINE;
    }

    [[nodiscard]]
    static auto num_possible_cpus() -> usize {
        return possible_mask().count();
    }

    [[nodiscard]]
    static auto num_present_cpus() -> usize {
        return present_mask().count();
    }

    [[nodiscard]]
    static auto num_online_cpus() -> usize {
        return online_mask().count();
    }

  private:
    static constexpr auto NUM_STATES = static_cast<usize>(State::Count);

    [[nodiscard]]
    static auto immutable_mask(State state) -> CpuMask & {
        return STATES[static_cast<usize>(state)];
    }

    /// Only Possible and Present live here; Online is ONLINE. The slot for
    /// Online is left in place so the array stays indexable by State and a
    /// mis-aimed read of it is a bug in this file rather than an out-of-bounds.
    static std::array<CpuMask, NUM_STATES> STATES;

    static std::atomic<CpuMask> ONLINE;
};

/// A CpuMask that is not lock-free would make every set_state() call a
/// libatomic function call, and there is no libatomic in this kernel -- it
/// would fail to link rather than run slowly. That is a real limit on
/// MP_MAX_CPUS (one 64-bit word today), so say it here instead of leaving it
/// to be discovered.
static_assert(std::atomic<CpuMask>::is_always_lock_free,
              "MP_MAX_CPUS is too large for a lock-free CPU mask");

inline auto cpu_possible_mask() -> CpuMask & {
    return Cpu::possible_mask();
}

/// The two constant masks, Linux's cpu_none_mask and cpu_all_mask.
///
/// They are returned by reference to a single static object because the
/// callers that want them keep a pointer -- a ClockEventDevice records
/// `CpuMask const *` -- and a per-call temporary would dangle. cpu_all_mask()
/// is "every CPU the kernel was built for", the bitset's full capacity, and
/// is deliberately wider than cpu_possible_mask(), which is what the
/// firmware said is present on this machine.
inline auto cpu_none_mask() -> CpuMask const & {
    static constexpr CpuMask NONE{};
    return NONE;
}

inline auto cpu_all_mask() -> CpuMask const & {
    static constexpr CpuMask ALL = CpuMask{}.set();
    return ALL;
}

} // namespace ours

#endif // OURS_CPU_HPP
